#ifndef H_GAME_TR
#define H_GAME_TR

//#define FREE_CAMERA

#include "core.h"
#include "format.h"
#include "level.h"

namespace Game {
    Level *level;

    void startLevel(Stream &stream, const char *sndName, bool demo, bool home) {
        delete level;
        level = new Level(stream, demo, home);

        #ifndef __EMSCRIPTEN__    
            //Sound::play(Sound::openWAD("05_Lara's_Themes.wav"), 1, 1, 0);
            Sound::play(new Stream(sndName), vec3(0.0f), 1, 1, Sound::Flags::LOOP);
            //Sound::play(new Stream("03.mp3"), 1, 1, 0);
        #endif            
    }

    void startLevel(const char *lvlName, const char *sndName, bool demo, bool home) {
        Stream stream(lvlName);
        startLevel(stream, sndName, demo, home);
    }
    
    void init(char *lvlName = NULL, char *sndName = NULL) {
        Core::init();
        level = NULL;
        
        if (!lvlName) lvlName = (char*)"LEVEL2.PSX";
        if (!sndName) sndName = (char*)"05.ogg";
        //lstartLevel("LEVEL2_DEMO.PHD", true, false);
        //lstartLevel("GYM.PSX", false, true);
        //lstartLevel("LEVEL3A.PHD", false, false);
        startLevel(lvlName, sndName, false, false);
    }
        
    void free() {
        delete level;
        Core::free();
    }

    void update() {
        float dt = Core::deltaTime;
        if (Input::down[ikR]) // slow motion (for animation debugging)
            Core::deltaTime /= 10.0f;
        if (Input::down[ikT])
            Core::deltaTime *= 10.0f;

        level->update();

        Core::deltaTime = dt;
    }

    void render() {
        level->render();
        Core::frameIndex++;
    }
}

#endif