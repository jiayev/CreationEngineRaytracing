#ifndef BYTE4_HLSL
#define BYTE4_HLSL

#include "Interop.h"

#include "interop/ubyte4.hlsli"

struct byte16f {
    ubyte16f value;
 
#ifdef __cplusplus
    void pack(half valueIn)
    {
        value.pack(valueIn * 0.5f + 0.5f);
    }  
#endif
    
    half unpack()
    {
        return value.unpack() * 2.0f - 1.0f;
    }  
};

struct byte4f {
    ubyte4f value;
    
#ifdef __cplusplus
    void pack(half4 valueIn)
    {
        value.pack(valueIn * 0.5f + float4(0.5f));
    }     
#endif
    
    half4 unpack()
    {
        return value.unpack() * 2.0f - float4(1.0f);
    } 
};
VALIDATE_SIZE(ubyte4f, 4);

#endif