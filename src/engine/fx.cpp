#include "cube.h"
#include "engine.h"

namespace fx
{
    VAR(0, fxdebug, 0, 0, 1);

    static instance *instances, *freeinstances;
    static emitter *emitters, *freeemitters, *activeemitters;
    static int numinstances, numemitters;

    static instance *getinstance()
    {
        instance *result = NULL;

        listpopfront(result, freeinstances, prev, next);
        if(result) numinstances++;

        if(fxdebug) conoutf("fx instance get: %p (num %d)", result, numinstances);

        return result;
    }

    static void putinstance(instance *inst)
    {
        inst->reset();

        listdremove(inst, inst->e->firstfx, inst->e->lastfx, prev, next);
        listpushfront(inst, freeinstances, prev, next);
        numinstances--;

        if(fxdebug) conoutf("fx instance put: %p (num %d)", inst, numinstances);
    }

    static emitter *getemitter()
    {
        emitter *result = NULL;

        listpopfront(result, freeemitters, prev, next);

        if(result)
        {
            listpushfront(result, activeemitters, prev, next);
            numemitters++;
        }

        if(fxdebug) conoutf("fx emitter get: %p (num %d)", result, numemitters);

        return result;
    }

    static void putemitter(emitter *e)
    {
        e->unhook();
        while(e->firstfx) putinstance(e->firstfx);

        listremove(e, activeemitters, prev, next);
        listpushfront(e, freeemitters, prev, next);

        numemitters--;
        if(fxdebug) conoutf("fx emitter put: %p (num %d)", e, numemitters);
    }

    VARF(IDF_PERSIST, maxfxinstances, 1, 2000, 10000, setup());
    VARF(IDF_PERSIST, maxfxemitters, 1, 500, 10000, setup());

    void instance::reset(bool initialize)
    {
        fxdef &def = getfxdef(fxindex);

        switch(def.type)
        {
            case FX_TYPE_SOUND:
                if(!initialize && issound(soundhook)) removesound(soundhook);
                soundhook = -1;
                break;

            case FX_TYPE_WIND: windhook = NULL; break;
        }
    }

    void instance::init(emitter *em, int index, instance *prnt)
    {
        e = em;
        fxindex = index;
        parent = prnt;
        sync = true;
        emitted = false;
        reset(true);
        calcactiveend();
        beginmillis = endmillis = lastmillis;
    }

    void instance::calcactiveend()
    {
        int interval = getprop<int>(FX_PROP_EMIT_INTERVAL);
        int length = interval ?
            getprop<int>(FX_PROP_ACTIVE_LENGTH) :
            getprop<int>(FX_PROP_EMIT_LENGTH);

        activeendmillis = lastmillis + length;
        e->updateend(activeendmillis);
    }

    void instance::calcend(int from)
    {
        int length = getprop<int>(FX_PROP_EMIT_LENGTH);
        endmillis = from + length;
    }

    void instance::prolong()
    {
        if(getprop<int>(FX_PROP_EMIT_SINGLE)) return;

        bool needsync = getprop<int>(FX_PROP_EMIT_PARENT) && parent;
        int interval = getprop<int>(FX_PROP_EMIT_INTERVAL);

        if(!needsync && !interval)
        {
            if(getprop<int>(FX_PROP_EMIT_RESTART)) beginmillis = lastmillis;
            calcend(lastmillis);
        }
        calcactiveend();
    }

    void instance::schedule(bool resync)
    {
        int delay = getprop<int>(FX_PROP_EMIT_DELAY);

        if(resync)
        {
            ASSERT(parent);

            beginmillis = parent->beginmillis + delay;
        }
        else
        {
            int interval = getprop<int>(FX_PROP_EMIT_INTERVAL);
            beginmillis = beginmillis + interval + delay;
        }

        calcend(beginmillis);

        if(getprop<int>(FX_PROP_EMIT_TIMELINESS) && endmillis > activeendmillis)
            beginmillis = endmillis = 0; // won't be able to finish in time, skip

        emitted = false;
    }

    void instance::update()
    {
        bool needsync = getprop<int>(FX_PROP_EMIT_PARENT) && parent;
        bool slip = false; // guarantee emit in case of frame slip ups

        if(endmillis) // set to 0 when skipping emission
        {
            if(needsync && parent->sync)
            {
                if(beginmillis != endmillis) slip = true;
                schedule(true);
            }
            else if(!needsync && lastmillis >= endmillis)
            {
                if(beginmillis != endmillis) slip = true;
                schedule(false);
                sync = true;
            }
        }

        if(slip || (lastmillis >= beginmillis && lastmillis < endmillis))
        {
            emitfx();
            emitted = true;
        }

        if(lastmillis >= activeendmillis && endmillis) stop();

        if(next) next->update();
        sync = false;
    }

    void instance::stop()
    {
        fxdef &def = getfxdef(fxindex);

        reset();

        if(def.endfx && isfx(def.endfx->index) && endmillis)
            createfx(def.endfx->index, e->from, e->to, e->blend, e->scale, e->color, e->pl, NULL);

        beginmillis = endmillis = 0;
    }

    void emitter::calcrandom() { rand = vec(rndscale(1), rndscale(1), rndscale(1)); }

    void emitter::unhook()
    {
        if(!hook) return;

        *hook = NULL;
        hook = NULL;
    }

    void emitter::updateend(int end) { endmillis = max(endmillis, end); }

    void emitter::init(emitter **newhook)
    {
        firstfx = lastfx = NULL;
        beginmillis = lastmillis;
        endmillis = 0; // will be calculated by instances
        pl = NULL;
        hook = newhook;
        if(hook) *hook = this;
        calcrandom();
    }

    void emitter::instantiate(int index, instance *parent)
    {
        if(!isfx(index))
        {
            conoutf("\frError: cannot instantiate fx, invalid index %d", index);
            return;
        }

        instance *inst = getinstance();

        if(!inst)
        {
            if(fxdebug) conoutf("\fyWarning: cannot instantiate fx, no free instances");
            return;
        }

        inst->init(this, index, parent);

        listdpushback(inst, firstfx, lastfx, prev, next);

        fxdef &def = getfxdef(index);
        loopv(def.children) instantiate(def.children[i], inst);
    }

    void emitter::prolong()
    {
        instance *inst = firstfx;
        while(inst)
        {
            inst->prolong();
            inst = inst->next;
        }
    }

    bool emitter::done() { return lastmillis >= endmillis; }

    void emitter::update()
    {
        game::fxtrack(this);

        calcrandom();
        firstfx->update();
        if(done()) putemitter(this);
    }

    void emitter::stop()
    {
        instance *inst = firstfx;
        while(inst)
        {
            inst->stop();
            inst = inst->next;
        }
    }

    static emitter *testemitter = NULL;
    static int testmillis;

    void update()
    {
        if(testemitter && lastmillis - testmillis < 10000) testemitter->prolong();

        emitter *e = activeemitters;
        while(e)
        {
            // store next now, current emitter might get reassigned during update
            emitter *next = e->next;
            e->update();
            e = next;
        }
    }

    void stopfx(emitter *e)
    {
        putemitter(e);
    }

    void createfx(int index, const vec &from, const vec &to, float blend, float scale,
        const bvec &color, physent *pl, emitter **hook)
    {
        // stop hooked FX if we want to make a different one under the same hook,
        // old hook is invalidated automatically
        if(hook && *hook && (*hook)->firstfx->fxindex != index) stopfx(*hook);

        emitter *e = hook && *hook ? *hook : getemitter();

        if(!e)
        {
            conoutf("\fyWarning: cannot create fx, no free emitters");
            return;
        }

        if(!e->hook)
        {
            e->init(hook);
            e->instantiate(index);
        }
        else e->prolong();

        e->from = from;
        e->to = to;
        e->blend = blend;
        e->scale = scale;
        e->color = color;
        e->pl = pl;
    }

    void clear() { while(activeemitters) putemitter(activeemitters); }

    ICOMMAND(0, clearfx, "", (), clear());

    void cleanup()
    {
        clear();
        DELETEA(instances);
        DELETEA(emitters);
    }

    void setup()
    {
        cleanup();
        instances = new instance[maxfxinstances];
        emitters = new emitter[maxfxemitters];

        listinit(instances, maxfxinstances, freeinstances, prev, next);
        listinit(emitters, maxfxemitters, freeemitters, prev, next);
    }

    ICOMMAND(0, testfx, "si", (char *name, int *sameinstance),
    {
        int fxindex = getfxindex(name);
        if(!isfx(fxindex)) return;

        vec dir;
        vec from;
        vec to;

        vecfromyawpitch(camera1->yaw, camera1->pitch, 1, 0, dir);
        raycubepos(camera1->o, dir, from);
        from.sub(dir.mul(16));
        to = vec(from).add(vec(0, 0, 32));

        createfx(fxindex, from, to, 1, 1, bvec(255, 255, 255), NULL, *sameinstance ? &testemitter : NULL);
        if(*sameinstance) testmillis = lastmillis;
    });
}
