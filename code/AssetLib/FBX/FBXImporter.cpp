/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2025, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.
r
* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

/** @file  FBXImporter.cpp
 *  @brief Implementation of the FBX importer.
 */

#ifndef ASSIMP_BUILD_NO_FBX_IMPORTER

#include "FBXImporter.h"

#include "FBXConverter.h"
#include "FBXDocument.h"
#include "FBXParser.h"
#include "FBXTokenizer.h"
#include "FBXUtil.h"

#include <assimp/MemoryIOWrapper.h>
#include <assimp/StreamReader.h>
#include <assimp/importerdesc.h>
#include <assimp/Importer.hpp>

namespace Assimp {

template <>
const char *LogFunctions<FBXImporter>::Prefix() {
	return "FBX: ";
}

} // namespace Assimp

using namespace Assimp;
using namespace Assimp::Formatter;
using namespace Assimp::FBX;

namespace {
    static constexpr aiImporterDesc desc = {
	    "Autodesk FBX Importer",
	    "",
	    "",
	    "",
	    aiImporterFlags_SupportTextFlavour,
	    0,
	    0,
	    0,
	    0,
	    "fbx"
    };
}

// ------------------------------------------------------------------------------------------------
// Returns whether the class can handle the format of the given file.
bool FBXImporter::CanRead(const std::string & pFile, IOSystem * pIOHandler, bool /*checkSig*/) const {
	// at least ASCII-FBX files usually have a 'FBX' somewhere in their head
	static const char *tokens[] = { "fbx" };
	return SearchFileHeaderForToken(pIOHandler, pFile, tokens, AI_COUNT_OF(tokens));
}

// ------------------------------------------------------------------------------------------------
// List all extensions handled by this loader
const aiImporterDesc *FBXImporter::GetInfo() const {
	return &desc;
}

// ------------------------------------------------------------------------------------------------
// Setup configuration properties for the loader
void FBXImporter::SetupProperties(const Importer *pImp) {
    mSettings.readAllLayers = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ALL_GEOMETRY_LAYERS, true);
    mSettings.readAllMaterials = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ALL_MATERIALS, false);
    mSettings.readMaterials = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_MATERIALS, true);
    mSettings.readTextures = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_TEXTURES, true);
    mSettings.readCameras = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_CAMERAS, true);
    mSettings.readLights = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_LIGHTS, true);
    mSettings.readAnimations = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ANIMATIONS, true);
    mSettings.readWeights = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_WEIGHTS, true);
    mSettings.strictMode = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_STRICT_MODE, false);
    mSettings.preservePivots = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, true);
    mSettings.optimizeEmptyAnimationCurves = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_OPTIMIZE_EMPTY_ANIMATION_CURVES, true);
    mSettings.useLegacyEmbeddedTextureNaming = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_EMBEDDED_TEXTURES_LEGACY_NAMING, false);
    mSettings.removeEmptyBones = pImp->GetPropertyBool(AI_CONFIG_IMPORT_REMOVE_EMPTY_BONES, true);
    mSettings.convertToMeters = pImp->GetPropertyBool(AI_CONFIG_FBX_CONVERT_TO_M, false);
    mSettings.ignoreUpDirection = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_IGNORE_UP_DIRECTION, false);
    mSettings.useSkeleton = pImp->GetPropertyBool(AI_CONFIG_FBX_USE_SKELETON_BONE_CONTAINER, false);
}

// ------------------------------------------------------------------------------------------------
// Imports the given file into the given scene structure.
void FBXImporter::InternReadFile(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
	auto streamCloser = [&](IOStream *pStream) {
		pIOHandler->Close(pStream);
	};
	std::unique_ptr<IOStream, decltype(streamCloser)> stream(pIOHandler->Open(pFile, "rb"), streamCloser);
	if (!stream) {
		ThrowException("Could not open file for reading");
	}

    ASSIMP_LOG_DEBUG("Reading FBX file");

	// read entire file into memory - no streaming for this, fbx
	// files can grow large, but the assimp output data structure
	// then becomes very large, too. Assimp doesn't support
	// streaming for its output data structures so the net win with
	// streaming input data would be very low.
	std::vector<char> contents;
	contents.resize(stream->FileSize() + 1);
	stream->Read(&*contents.begin(), 1, contents.size() - 1);
	contents[contents.size() - 1] = 0;
	const char *const begin = &*contents.begin();

	// broad-phase tokenized pass in which we identify the core
	// syntax elements of FBX (brackets, commas, key:value mappings)
	TokenList tokens;
    Assimp::StackAllocator tempAllocator;
    try {
		bool is_binary = false;
		if (!strncmp(begin, "Kaydara FBX Binary", 18)) {
			is_binary = true;
            TokenizeBinary(tokens, begin, contents.size(), tempAllocator);
		} else {
            Tokenize(tokens, begin, tempAllocator);
		}

		// use this information to construct a very rudimentary
		// parse-tree representing the FBX scope structure
        Parser parser(tokens, tempAllocator, is_binary);

		// take the raw parse-tree and convert it to a FBX DOM
		Document doc(parser, mSettings);

		// convert the FBX DOM to aiScene
		ConvertToAssimpScene(pScene, doc, mSettings.removeEmptyBones);

		// size relative to cm
		float size_relative_to_cm = doc.GlobalSettings().UnitScaleFactor();
        if (size_relative_to_cm == 0.0) {
			// BaseImporter later asserts that fileScale is non-zero.
			ThrowException("The UnitScaleFactor must be non-zero");
        }

		// Set FBX file scale is relative to CM must be converted to M for
		// assimp universal format (M)
		SetFileScale(size_relative_to_cm * 0.01f);

		// This collection does not own the memory for the tokens, but we need to call their d'tor
        std::for_each(tokens.begin(), tokens.end(), Util::destructor_fun<Token>());

    } catch (std::exception &) {
        std::for_each(tokens.begin(), tokens.end(), Util::destructor_fun<Token>());
        throw;
	}
}

#endif // !ASSIMP_BUILD_NO_FBX_IMPORTER
