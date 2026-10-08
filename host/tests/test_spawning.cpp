#include "testing.h"
#include "mc/server/server.h"

using namespace mc;

TEST(spawn_sky_darkening_follows_the_day) {
    CHECK_EQ(skyDarkening(6000, false, false), 0);    // noon
    CHECK_EQ(skyDarkening(18000, false, false), 11);  // midnight
    CHECK(skyDarkening(13000, false, false) > 4);     // dusk
    CHECK(skyDarkening(6000, true, false) > 0);       // rain darkens the day
    CHECK(skyDarkening(6000, true, true) > skyDarkening(6000, true, false));
}

TEST(spawn_monsters_need_darkness) {
    Rng r(7);
    int dark = 0, lit = 0, day = 0, night = 0;
    for (int i = 0; i < 1000; i++) {
        dark += darkEnoughForMonster(0, 0, 0, false, r);        // a sealed cave
        lit += darkEnoughForMonster(0, 14, 11, false, r);       // next to a torch
        day += darkEnoughForMonster(15, 0, 0, false, r);        // open sky at noon
        night += darkEnoughForMonster(15, 0, 11, false, r);     // open sky at midnight
    }
    CHECK_EQ(dark, 1000);
    CHECK_EQ(lit, 0);
    CHECK_EQ(day, 0);
    CHECK(night > 0 && night < 1000);   // sky light 15 passes 1 in 2 (> random(32)); then 4 <= random(8)
    printf("    open sky at midnight passes %d of 1000\n", night);
}

TEST(spawn_animals_need_light) {
    CHECK(brightEnoughForAnimal(15, 0));
    CHECK(brightEnoughForAnimal(0, 9));
    CHECK(!brightEnoughForAnimal(8, 8));
}
