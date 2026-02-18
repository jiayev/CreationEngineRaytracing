#pragma once

namespace Util
{
	namespace Traversal
	{

		static RE::BSVisit::BSVisitControl ScenegraphFadeNodes(RE::NiAVObject* a_object, std::function<RE::BSVisit::BSVisitControl(RE::BSFadeNode*)> a_func)
		{
			auto result = RE::BSVisit::BSVisitControl::kContinue;

			if (!a_object) {
				return result;
			}

			auto fadeNode = a_object->AsFadeNode();
			if (fadeNode) {
				result = a_func(fadeNode);

				if (result == RE::BSVisit::BSVisitControl::kStop) {
					return result;
				}
			}

			auto node = a_object->AsNode();
			if (node) {
				for (auto& child : node->GetChildren()) {
					result = ScenegraphFadeNodes(child.get(), a_func);
					if (result == RE::BSVisit::BSVisitControl::kStop) {
						break;
					}
				}
			}

			return result;
		}

		// A custom visit controller built to ignore billboard/particle geometry
		static RE::BSVisit::BSVisitControl ScenegraphRTGeometries(RE::NiAVObject* a_object, RE::BSFadeNode* validFadeNode, std::function<RE::BSVisit::BSVisitControl(RE::BSGeometry*)> a_func)
		{
			auto result = RE::BSVisit::BSVisitControl::kContinue;

			if (!a_object) {
				return result;
			}

			auto geom = a_object->AsGeometry();
			if (geom) {
				return a_func(geom);
			}

			// Doodlum sez this is faster
			auto rtti = a_object->GetRTTI();

			static REL::Relocation<const RE::NiRTTI*> billboardRTTI{ RE::NiBillboardNode::Ni_RTTI };
			if (rtti == billboardRTTI.get())
				return result;

			// Might break vegetation
			static REL::Relocation<const RE::NiRTTI*> orderedRTTI{ RE::BSOrderedNode::Ni_RTTI };
			if (rtti == orderedRTTI.get())
				return result;

			auto node = a_object->AsNode();
			if (node) {
				for (auto& child : node->GetChildren()) {
					if (!child)
						continue;

					if (validFadeNode) {
						if (auto fadeNode = child->AsFadeNode(); fadeNode && fadeNode != validFadeNode) {
							continue;
						}
					}

					result = ScenegraphRTGeometries(child.get(), validFadeNode, a_func);
					if (result == RE::BSVisit::BSVisitControl::kStop) {
						break;
					}
				}
			}

			return result;
		}

	}
}