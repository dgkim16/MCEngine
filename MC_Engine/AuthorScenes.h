#pragma once

// One-shot scene-JSON author tool. Compiled only when MC_AUTHOR_SCENES_ONCE is
// defined; the entry point is invoked from MCEngine::Initialize before LoadAll
// and immediately followed by std::exit(0). Once a run produces the v2-shape
// JSONs in Assets/scenes/, commit them and remove the -DMC_AUTHOR_SCENES_ONCE
// from the project. The program is a build-time data generator; it has no
// place at runtime and no place after the JSONs are committed.
//
// Per Step 8A4: scene-specific knowledge here is acceptable because this code
// produces data, not runtime behavior. Do not be tempted to keep it around as
// a "scene factory" — that's the antipattern in disguise.

#ifdef MC_AUTHOR_SCENES_ONCE

namespace AuthorScenes {
	// Reads existing v1-shape Assets/scenes/scene_*.json, transforms them into
	// the v2 eight-key shape, and writes them back. Throws on file-system
	// errors or unrecognized v1 SubmeshName values.
	void EmitAll();
}

#endif
