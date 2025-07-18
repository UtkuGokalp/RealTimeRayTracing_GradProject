//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

//This shader file has nothing to do with ray tracing.
//At the beginning of this project, rasterization rendering was also planned to be implemented.
//Due to time constraints, that decision was scrapped. This shader file is for the scrapped rasterization pipeline.

struct PSInput
{
	float4 position : SV_POSITION;
	float4 color : COLOR;
};

struct InstanceProperties
{
    float4x4 objectToWorld;
	// #DXR Extra - Simple Lighting
    float4x4 objectToWorldNormal; //Not used here, used for raytracing. Here only for alignment.
};

StructuredBuffer<InstanceProperties> instanceProps : register(t0);

//The necessary matrices for rasterizer are in the beginning of the
//camera buffer, so we only declare those
cbuffer CameraParams : register(b0)
{
    float4x4 view;
    float4x4 projection;
}

uint instanceIndex : register(b1);

PSInput VSMain(float4 position : POSITION, float4 color : COLOR)
{
	PSInput result;

	// #DXR Extra: Perspective Camera
	
    float4 pos = mul(instanceProps[instanceIndex].objectToWorld, position);
    pos = mul(view, pos);
    pos = mul(projection, pos);
	result.position = pos;
	result.color = color;

	return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	return input.color;
}
