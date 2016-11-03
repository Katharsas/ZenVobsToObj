/*
The MIT License(MIT)

Copyright(c) 2016, ZenToObj.cpp authors :
 - degenerated1123
 - Jan Mothes

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <vdfs/fileIndex.h>
#include <zenload/zenParser.h>
#include <zenload/zCMesh.h>
#include <iostream>
#include <utils/export.h>
#include <map>
#include <zenload/zCProgMeshProto.h>
#include <lib/glm/glm/detail/type_mat.hpp>
#include <lib/glm/glm/detail/type_mat4x4.hpp>
#include <daedalus/DATFile.h>
#include <zenload/zTypes.h>
#include <daedalus/DaedalusVM.h>
#include <tinydir.h>

const bool g_isExportedVerticesLimited = false;// for debugging
const size_t g_exportedVerticesLimit = 100000;

std::string getItemVisual(std::string& instanceName, Daedalus::DATFile& datFile, Daedalus::DaedalusVM* vm) {
	
	if (datFile.hasSymbolName(instanceName))
	{
		Daedalus::PARSymbol& s = datFile.getSymbolByName(instanceName);

		Daedalus::GameState::ItemHandle hitem = vm->getGameState().insertItem(instanceName);
		Daedalus::GEngineClasses::C_Item& item = vm->getGameState().getItem(hitem);

		return item.visual;
	}
	else return "";
}

/**
 * Recursive function to collect all vobs with their visual names.
 * Mobs are currently not supported.
 */
void collectVobsWithVisuals(
	std::vector<ZenLoad::zCVobData>& vobs, std::vector<ZenLoad::zCVobData>& target, Daedalus::DATFile& datFile, Daedalus::DaedalusVM* vm)
{
	for (ZenLoad::zCVobData& vob : vobs)
	{
		// if item, visual must be retrieved from item description daedalus script
		if (vob.objectClass.find("oCItem") != std::string::npos)
		{
			getItemVisual(vob.oCItem.instanceName, datFile, vm);
		}

		if (!vob.visual.empty() && vob.visual.find(".3DS") != std::string::npos)
		{
			target.push_back(vob);
		}

		// List the visuals of the children as well
		collectVobsWithVisuals(vob.childVobs, target, datFile, vm);
	}
}

/**
 * Assumes all vdf files have only ASCII symbols in filename.
 */
void loadAllVdfFiles(std::string& vdfParentDir, VDFS::FileIndex& vdf) {

	tinydir_dir dir;
	if (tinydir_open(&dir, vdfParentDir.c_str()) == -1)
	{
		std::cout << "Failed to open directory: " << vdfParentDir << std::endl;
		tinydir_close(&dir);
		return;
	}

	while (dir.has_next)
	{
		tinydir_file file;
		if (tinydir_readfile(&dir, &file) == -1)
		{
			std::cout << "Error getting file" << std::endl;
			tinydir_close(&dir);
			return;
		}
		std::string ext = file.extension;
		// assumes pure ASCII chars, does not work for all unicode codepoints
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext == "vdf") {
			vdf.loadVDF(vdfParentDir + file.name);
		}
		tinydir_next(&dir);
	}

	tinydir_close(&dir);
}

/**
 * Find visuals, extract mesh data, transform local vertices' positions to global positions, merge.
 */
void transformAndMerge(
	std::vector<ZenLoad::zCVobData>& vobsWithVisuals, ZenLoad::PackedMesh& targetScene, VDFS::FileIndex& vdf) {
	
	// mapping from visual name to its transformed mesh for all visuals of collected vobs (cache)
	std::map<std::string, ZenLoad::PackedMesh> meshes;
	
	for (ZenLoad::zCVobData& vob : vobsWithVisuals)
	{
		// mesh of current visual
		ZenLoad::PackedMesh mesh;

		// if not in cache, find mesh
		if (meshes.find(vob.visual) == meshes.end())
		{
			std::string filename = vob.visual.substr(0, vob.visual.find_last_of('.')) + ".MRM";

			ZenLoad::zCProgMeshProto rawMesh(filename, vdf);

			// .mrm does not exist for worldmesh parts, so rawMesh will be empty for those
			if (rawMesh.getNumSubmeshes() == 0)
			{
				std::cout << "Skipping worldmesh visual: " << vob.visual << std::endl;
				continue;
			}
			// create mesh and add to cache
			rawMesh.packMesh(mesh);
			meshes[vob.visual] = mesh;
		}
		else {
			// get from cache
			mesh = meshes[vob.visual];
		}

		// Empty visuals will not be added to scene
		if (mesh.vertices.empty()) {
			continue;
		}

		// Transform vertex position and add to scene
		size_t offset = targetScene.vertices.size();
		for (ZenLoad::WorldVertex vertex : mesh.vertices)
		{
			glm::mat4 worldMatrix = *(glm::mat4*)&vob.worldMatrix;
			glm::vec3 vertexPos = *(glm::vec3*)&vertex.Position;
			glm::vec4 transformed = worldMatrix * glm::vec4(vertexPos, 1.0f);

			vertex.Position =
				ZMath::float3(transformed.x * 0.01f, transformed.y * 0.01f, transformed.z * 0.01f);
			targetScene.vertices.push_back(vertex);
		}

		for (auto& subMesh : mesh.subMeshes)
		{
			for (uint32_t idx : subMesh.indices)
			{
				targetScene.subMeshes[0].indices.push_back((uint32_t)(idx + offset));
			}
		}

		// for faster debugging, limit amount of verts
		if (g_isExportedVerticesLimited && targetScene.vertices.size() > g_exportedVerticesLimit) {
			break;
		}
	}
}

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		std::cout
			<< "Usage: zen_load <zen-name> <gothic-path>" << std::endl
			<< "       <zen-name>: Zen whoose vobs to export to .obj (G1: world.zen, G2:newworld.zen)" << std::endl
			<< "       <gothic-path>: Path to Gothic game directory (which contains data and _work)" << std::endl
			<< std::endl
			<< "This tool will scan the data folder for all vdfs and export as many vobs as possible." << std::endl;
		return 0;
	}

	std::string gamePath = argv[2];

	// Create file-index to load our vdf-archive
	VDFS::FileIndex vdf;
	std::string vdfPath = gamePath + "/Data/";

	// Load VDFs
	loadAllVdfFiles(vdfPath, vdf);

	// Initialize parser with zenfile from vdf
	ZenLoad::ZenParser parser(argv[1], vdf);

	if (parser.getFileSize() == 0)
	{
		std::cout << "Error: ZEN-File either not found or empty!" << std::endl;
		return 0;
	}

	// Since this is a usual level-zen, read the file header
	// You will most likely allways need to do that
	parser.readHeader();

	// Do something with the header, if you want.
	std::cout <<"Reading ZEN:" << std::endl
		<< "	Author: " << parser.getZenHeader().user << std::endl
		<< "	Date: " << parser.getZenHeader().date << std::endl
		<< "	Object-count: " << parser.getZenHeader().objectCount << std::endl;

	// Read the rest of the ZEN-file
	ZenLoad::oCWorldData world;
	parser.readWorld(world);

	std::cout << "Done reading ZEN!" << std::endl;

	// collect vobs which have an exportable visual (items & static)
	std::vector<ZenLoad::zCVobData> vobsWithVisuals;
	{
		// Prepare reading of Daedalus item scripts
		Daedalus::DATFile dat = Daedalus::DATFile(gamePath + "_work/DATA/scripts/_compiled/GOTHIC.DAT");
		Daedalus::DaedalusVM* vm = new Daedalus::DaedalusVM(dat);

		vm->getGameState().registerExternals();
		Daedalus::registerDaedalusStdLib(*vm);
		Daedalus::registerGothicEngineClasses(*vm);

		collectVobsWithVisuals(world.rootVobs, vobsWithVisuals, dat, vm);
	}

	// vob meshes will be merged into single scene mesh
	ZenLoad::PackedMesh scene = {};
	scene.subMeshes.resize(1);

	transformAndMerge(vobsWithVisuals, scene, vdf);

	std::cout << " \nExporting..." << std::endl;

	// If this function call crashes, it's probably because of a bad_alloc which happens when
	// not enough memory is available in a single block. Try largeAdressAware or 64 bit build...
	Utils::exportPackedMeshToObj(scene, (argv[1] + std::string(".OBJ")), 6);

	return EXIT_SUCCESS;
}

