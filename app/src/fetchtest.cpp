// Standalone CLI harness for gdapp::fetchLevel — reuses the exact same code
// path as the app, for debugging without needing to click through the UI.
#include "leveldata.hpp"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    int id = argc > 1 ? std::atoi(argv[1]) : 128;
    printf("fetching level %d...\n", id);
    gdapp::LevelFetchResult r = gdapp::fetchLevel(id);
    if (r.success) {
        printf("OK: name=\"%s\" levelString length=%zu\n", r.name.c_str(), r.levelString.size());
        printf("first 120 chars: %.120s\n", r.levelString.c_str());
    } else {
        printf("FAILED: %s\n", r.error.c_str());
    }
    return r.success ? 0 : 1;
}
