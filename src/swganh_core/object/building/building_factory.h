// This file is part of SWGANH which is released under the MIT license.
// See file LICENSE or go to http://swganh.com/LICENSE
#pragma once

#include "swganh_core/object/tangible/tangible_factory.h"

namespace swganh {
namespace object {

	class ObjectManager;



	class Building;

    class BuildingFactory : public swganh::object::TangibleFactory
    {
    public:
		typedef Building ObjectType;

        BuildingFactory(swganh::app::SwganhKernel* kernel);
		
		virtual void PersistChangedObjects(){}
		virtual void RegisterEventHandlers(){}

		std::shared_ptr<swganh::object::Object> CreateObject();

    };

}}  // namespace swganh::object
