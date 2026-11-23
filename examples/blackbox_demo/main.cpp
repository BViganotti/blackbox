// Minimal embedder: start the recorder, the demo service and the local UI.
// The same entry point powers the blackbox_launcher binary.
#include <blackbox/demo.hpp>

int main(int argc, char** argv) { return bb::run_demo_app(argc, argv); }
