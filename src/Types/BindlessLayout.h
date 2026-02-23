#pragma once

class BindlessLayout : public nvrhi::RefCounter<nvrhi::IBindingLayout>
{
public:
	nvrhi::BindlessLayoutDesc desc;
	nvrhi::static_vector<D3D12_DESCRIPTOR_RANGE1, 32> descriptorRanges;
	D3D12_ROOT_PARAMETER1 rootParameter{};

	BindlessLayout(const nvrhi::BindlessLayoutDesc& desc);

	const nvrhi::BindingLayoutDesc* getDesc() const override { return nullptr; }
	const nvrhi::BindlessLayoutDesc* getBindlessDesc() const override { return &desc; }
};