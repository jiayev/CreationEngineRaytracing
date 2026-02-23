#pragma once

struct BindlessTable
{
	nvrhi::BindingLayoutHandle m_Layout;
	eastl::shared_ptr<DescriptorTableManager> m_DescriptorTable;

	BindlessTable(nvrhi::DeviceHandle device, nvrhi::BindlessLayoutDesc desc, bool resizeToMaxCapacity)
	{
		m_Layout = device->createBindlessLayout(desc);
		m_DescriptorTable = eastl::make_shared<DescriptorTableManager>(device, m_Layout, resizeToMaxCapacity);
	}
};