#include "core/Model.h"
#include "Scene.h"

void Model::BuildBLAS()
{
	auto scene = Scene::GetSingleton();

	auto& nvrhiDevice = scene->m_NVRHIDevice;
	auto& commandList = scene->m_CommandList;

	blasDesc.setDebugName("BLAS")
		.setIsTopLevel(false);

	// Initial build with all shapes, visible or not, so the scratch buffer can be sized to fit all geometry
	for (size_t i = 0; i < meshes.size(); i++) {
		blasDesc.addBottomLevelGeometry(meshes[i]->geometryDesc);
	}

	blas = nvrhiDevice->createAccelStruct(blasDesc);

	auto& geometries = blasDesc.bottomLevelGeometries;

	commandList->buildBottomLevelAccelStruct(blas, geometries.data(), geometries.size());
}