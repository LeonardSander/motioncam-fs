#include "VulkanJpegFullEncode.h"

#ifdef MOTIONCAM_HAS_DIRECTLOG_PLACEBO
#include "VulkanJpegContext.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <libplacebo/gpu.h>
#include <libplacebo/vulkan.h>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace motioncam {
namespace {
constexpr uint32_t kBlock = 256;
constexpr uint32_t kMetaWords = 4;
constexpr char histogramShader[] = R"glsl(#version 450
layout(local_size_x = 256) in;
#ifdef JPEG_SRC_TEXTURE
layout(binding = 0) uniform sampler2D srcTex;
#else
layout(std430, binding = 0) readonly buffer Source { uint words[]; } src;
#endif
layout(std430, binding = 1) buffer Counts { uint values[]; } hist;
layout(push_constant) uniform Constants {
    uint width; uint height; uint channels; uint bits;
    uint sourceBits; uint wordsPerRow;
} p;
shared uint localCounts[8*18];
#ifdef JPEG_SRC_TEXTURE
uint sampleAt(uint i) {
#ifdef JPEG_SRC_RGB
    uint pixel=i/3u, component=i%3u;
    ivec2 pos=ivec2(int(pixel%p.width),int(pixel/p.width));
    vec4 color=texelFetch(srcTex,pos,0);
    float componentValue=component==0u?color.r:component==1u?color.g:color.b;
    return uint(round(componentValue*65535.0));
#else
    ivec2 pos=ivec2(int(i%p.width),int(i/p.width));
    return uint(round(texelFetch(srcTex,pos,0).r*65535.0));
#endif
}
#else
uint sampleAt(uint i) {
    if(p.sourceBits==16u && p.wordsPerRow==0u) {
        uint v=src.words[i>>1u]; return (i&1u)==0u ? v&65535u : v>>16u;
    }
    uint rowSamples=p.width*p.channels;
    if(p.sourceBits==16u) {
        uint index=i%rowSamples;
        uint v=src.words[(i/rowSamples)*p.wordsPerRow+(index>>1u)];
        return (index&1u)==0u?v&65535u:v>>16u;
    }
    uint bit=(i%rowSamples)*p.sourceBits;
    uint byteIndex=(i/rowSamples)*p.wordsPerRow*4u+(bit>>3u);
    uint totalBytes=p.wordsPerRow*p.height*4u;
    uint first=src.words[byteIndex>>2u]>>((byteIndex&3u)*8u);
    uint second=byteIndex+1u<totalBytes?
        src.words[(byteIndex+1u)>>2u]>>(((byteIndex+1u)&3u)*8u):0u;
    uint third=byteIndex+2u<totalBytes?
        src.words[(byteIndex+2u)>>2u]>>(((byteIndex+2u)&3u)*8u):0u;
    uint window=((first&255u)<<16u)|((second&255u)<<8u)|(third&255u);
    return (window>>(24u-p.sourceBits-(bit&7u)))&((1u<<p.sourceBits)-1u);
}
#endif
int predict(uint mode, int a, int b, int c) {
    if(mode==1u) return a; if(mode==2u) return b; if(mode==3u) return c;
    if(mode==4u) return a+b-c; if(mode==5u) return a+((b-c)>>1);
    if(mode==6u) return b+((a-c)>>1); return (a+b)>>1;
}
void countResiduals(int v,int a,int b,int c,uint row,uint x) {
    for(uint mode=1u;mode<=7u;++mode) {
        int pred=row==0u&&x==0u?int(1u<<(p.bits-1u)):
                 row==0u?a:x==0u?b:predict(mode,a,b,c);
        int d=(v-pred)&65535; if(d>=32768) d-=65536;
        uint m=uint(abs(d)); uint cat=m==0u?0u:uint(findMSB(m)+1);
        atomicAdd(localCounts[mode*18u+cat],1u);
    }
}
void main() {
    uint lane=gl_LocalInvocationID.x;
    if(lane<8u*18u)localCounts[lane]=0u;
    barrier();
#ifdef JPEG_SRC_RGB
    uint task=gl_GlobalInvocationID.x;
    uint row=0u, x=0u;
    if(p.width*p.height*3u>=1000000u) {
        // Map threads directly to the 2x2 cells chosen by the predictor sample.
        uint tasksPerRow=((p.width+15u)/16u)*2u;
        row=task/tasksPerRow;
        uint position=task%tasksPerRow;
        uint cell=(8u-(((row>>1u)*3u)&7u))&7u;
        x=(cell+8u*(position>>1u))*2u+(position&1u);
    } else {
        row=task/p.width;
        x=task%p.width;
    }
    if(row<p.height && x<p.width) {
            ivec2 position=ivec2(int(x),int(row));
            vec4 v=texelFetch(srcTex,position,0);
            vec4 a=x>0u?texelFetch(srcTex,position+ivec2(-1,0),0):vec4(0.0);
            vec4 b=row>0u?texelFetch(srcTex,position+ivec2(0,-1),0):vec4(0.0);
            vec4 c=row>0u&&x>0u?
                texelFetch(srcTex,position+ivec2(-1,-1),0):vec4(0.0);
            for(uint component=0u;component<3u;++component)
                countResiduals(int(round(v[component]*65535.0)),
                               int(round(a[component]*65535.0)),
                               int(round(b[component]*65535.0)),
                               int(round(c[component]*65535.0)),row,x);
    }
#else
    uint i=gl_GlobalInvocationID.x, rowSamples=p.width*p.channels;
    if(i<rowSamples*p.height) {
        uint row=i/rowSamples, x=(i%rowSamples)/p.channels;
        if(rowSamples*p.height<1000000u ||
           ((((row>>1u)*3u+(x>>1u))&7u)==0u)) {
        int v=int(sampleAt(i));
        int a=x>0u?int(sampleAt(i-p.channels)):0;
        int b=row>0u?int(sampleAt(i-rowSamples)):0;
        int c=row>0u&&x>0u?int(sampleAt(i-rowSamples-p.channels)):0;
        countResiduals(v,a,b,c,row,x);
        }
    }
#endif
    barrier();
    if(lane<8u*18u && localCounts[lane]!=0u)
        atomicAdd(hist.values[lane],localCounts[lane]);
}
)glsl";
constexpr char tableShader[] = R"glsl(#version 450
layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer Counts { uint values[]; } hist;
layout(std430, binding = 1) writeonly buffer Table { uint values[]; } outTable;
layout(push_constant) uniform Constants { uint samples; } p;
void main() {
    uint best=1u; uint bestCost=0xffffffffu;
    for(uint mode=1u;mode<=7u;++mode) {
        uint cost=0u;
        for(uint cat=1u;cat<=16u;++cat) cost+=cat*hist.values[mode*18u+cat];
        if(cost<bestCost) { bestCost=cost; best=mode; }
    }
    outTable.values[0]=best;
    float freq[18]; int size[18]; int others[18];
    float total=0.0;
    for(int i=0;i<17;++i) total+=float(max(hist.values[best*18u+uint(i)],
                                                p.samples>=1000000u?1u:0u));
    for(int i=0;i<17;++i) {
        uint n=max(hist.values[best*18u+uint(i)],p.samples>=1000000u?1u:0u);
        freq[i]=float(n)/total; size[i]=0; others[i]=-1;
    }
    freq[17]=1.0; size[17]=0; others[17]=-1;
    for(int iteration=0;iteration<17;++iteration) {
        float f1=3.0, f2=3.0; int v1=-1,v2=-1;
        for(int i=0;i<18;++i) if(freq[i]<=f1 && freq[i]>0.0) { f1=freq[i]; v1=i; }
        for(int i=0;i<18;++i) if(i!=v1 && freq[i]<f2 && freq[i]>0.0) { f2=freq[i]; v2=i; }
        if(v2<0) break;
        freq[v1]+=freq[v2]; freq[v2]=0.0;
        for(int j=0;j<18;++j) { size[v1]++; if(others[v1]<0) break; v1=others[v1]; }
        others[v1]=v2;
        for(int j=0;j<18;++j) { size[v2]++; if(others[v2]<0) break; v2=others[v2]; }
    }
    int bits[18]; for(int i=0;i<18;++i) bits[i]=0;
    bool valid=true;
    for(int i=0;i<18;++i) if(size[i]>0) {
        if(size[i]>17) valid=false; else bits[size[i]]++;
    }
    if(!valid) { outTable.values[1]=0u; return; }
    int I=17;
    for(int guard=0;guard<18;++guard) {
        if(bits[I]>0) {
            int J=I-2;
            while(J>=0 && bits[J]<=0) --J;
            if(J<0 || bits[I]<2) { valid=false; break; }
            bits[I]-=2; bits[I-1]++; bits[J+1]+=2; bits[J]--;
        } else {
            --I;
            if(I!=16) continue;
            while(I>0 && bits[I]==0) --I;
            if(I==0) { valid=false; break; }
            bits[I]--; break;
        }
    }
    if(!valid) { outTable.values[1]=0u; return; }
    int huffval[17]; for(int i=0;i<17;++i)huffval[i]=0;
    int nvals=0;
    for(int len=1;len<=32;++len)
        for(int sym=0;sym<17;++sym)
            if(size[sym]==len) huffval[nvals++]=sym;
    uint code=0u; int index=0;
    for(int len=1;len<=16;++len) {
        outTable.values[2u+uint(len-1)]=uint(bits[len]);
        for(int j=0;j<bits[len];++j) {
            if(index>=nvals) { valid=false; break; }
            int sym=huffval[index++];
            outTable.values[35u+uint(sym)]=code;
            outTable.values[52u+uint(sym)]=uint(len);
            ++code;
        }
        code<<=1u;
    }
    for(int i=0;i<17;++i) outTable.values[18u+uint(i)]=uint(huffval[i]);
    outTable.values[1]=valid && index==nvals?uint(nvals):0u;
}
)glsl";
constexpr char packShader[] = R"glsl(#version 450
layout(local_size_x = 16) in;
#ifdef JPEG_SRC_TEXTURE
layout(binding = 0) uniform sampler2D srcTex;
#else
layout(std430, binding = 0) readonly buffer Source { uint words[]; } src;
#endif
layout(std430, binding = 1) readonly buffer Table { uint values[]; } table;
layout(std430, binding = 2) writeonly buffer Packed { uint words[]; } packed;
layout(std430, binding = 3) writeonly buffer Lengths { uint bits[]; } lengths;
layout(push_constant) uniform Constants {
    uint samples; uint stride; uint width; uint height; uint channels; uint bits;
    uint sourceBits; uint wordsPerRow;
} p;
#ifdef JPEG_SRC_TEXTURE
uint sampleAt(uint i) {
#ifdef JPEG_SRC_RGB
    uint pixel=i/3u, component=i%3u;
    ivec2 pos=ivec2(int(pixel%p.width),int(pixel/p.width));
    vec4 color=texelFetch(srcTex,pos,0);
    float componentValue=component==0u?color.r:component==1u?color.g:color.b;
    return uint(round(componentValue*65535.0));
#else
    ivec2 pos=ivec2(int(i%p.width),int(i/p.width));
    return uint(round(texelFetch(srcTex,pos,0).r*65535.0));
#endif
}
#else
uint sampleAt(uint i) {
    if(p.sourceBits==16u && p.wordsPerRow==0u) {
        uint v=src.words[i>>1u]; return (i&1u)==0u?v&65535u:v>>16u;
    }
    uint rowSamples=p.width*p.channels;
    if(p.sourceBits==16u) {
        uint index=i%rowSamples;
        uint v=src.words[(i/rowSamples)*p.wordsPerRow+(index>>1u)];
        return (index&1u)==0u?v&65535u:v>>16u;
    }
    uint bit=(i%rowSamples)*p.sourceBits;
    uint byteIndex=(i/rowSamples)*p.wordsPerRow*4u+(bit>>3u);
    uint totalBytes=p.wordsPerRow*p.height*4u;
    uint first=src.words[byteIndex>>2u]>>((byteIndex&3u)*8u);
    uint second=byteIndex+1u<totalBytes?
        src.words[(byteIndex+1u)>>2u]>>(((byteIndex+1u)&3u)*8u):0u;
    uint third=byteIndex+2u<totalBytes?
        src.words[(byteIndex+2u)>>2u]>>(((byteIndex+2u)&3u)*8u):0u;
    uint window=((first&255u)<<16u)|((second&255u)<<8u)|(third&255u);
    return (window>>(24u-p.sourceBits-(bit&7u)))&((1u<<p.sourceBits)-1u);
}
#endif
int predict(uint mode,int a,int b,int c) {
    if(mode==1u)return a; if(mode==2u)return b; if(mode==3u)return c;
    if(mode==4u)return a+b-c; if(mode==5u)return a+((b-c)>>1);
    if(mode==6u)return b+((a-c)>>1); return (a+b)>>1;
}
void main() {
    uint block=gl_GlobalInvocationID.x, first=block*256u;
    if(first>=p.samples) return;
    if(table.values[1]==0u) { lengths.bits[block]=0u; return; }
    uint end=min(first+256u,p.samples), rowSamples=p.width*p.channels;
    uint current=0u, word=0u, pending=0u, total=0u;
    uint mode=table.values[0];
#ifdef JPEG_SRC_RGB
    uint row=0u, x=0u;
    vec4 rgbValue=vec4(0.0), rgbLeft=vec4(0.0);
    vec4 rgbAbove=vec4(0.0), rgbDiagonal=vec4(0.0);
#endif
    for(uint i=first;i<end;++i) {
#ifdef JPEG_SRC_RGB
        uint component=i%3u;
        if(i==first || component==0u) {
            uint pixel=i/3u;
            row=pixel/p.width;
            x=pixel%p.width;
            ivec2 position=ivec2(int(x),int(row));
            rgbValue=texelFetch(srcTex,position,0);
            rgbLeft=x>0u?texelFetch(srcTex,position+ivec2(-1,0),0):vec4(0.0);
            rgbAbove=row>0u?texelFetch(srcTex,position+ivec2(0,-1),0):vec4(0.0);
            rgbDiagonal=row>0u&&x>0u?
                texelFetch(srcTex,position+ivec2(-1,-1),0):vec4(0.0);
        }
        int v=int(round(rgbValue[component]*65535.0));
        int a=int(round(rgbLeft[component]*65535.0));
        int b=int(round(rgbAbove[component]*65535.0));
        int c=int(round(rgbDiagonal[component]*65535.0));
#else
        uint row=i/rowSamples, x=(i%rowSamples)/p.channels;
        int v=int(sampleAt(i));
        int a=x>0u?int(sampleAt(i-p.channels)):0;
        int b=row>0u?int(sampleAt(i-rowSamples)):0;
        int c=row>0u&&x>0u?int(sampleAt(i-rowSamples-p.channels)):0;
#endif
        int pred=row==0u&&x==0u?int(1u<<(p.bits-1u)):
                 row==0u?a:x==0u?b:predict(mode,a,b,c);
        int diff=(v-pred)&65535; if(diff>=32768)diff-=65536;
        uint mag=uint(abs(diff)); uint cat=mag==0u?0u:uint(findMSB(mag)+1);
        uint extra=cat==16u?0u:cat, huff=table.values[35u+cat];
        uint hbits=table.values[52u+cat];
        uint threshold=cat==0u?0u:1u<<(cat-1u);
        if(diff<int(threshold))diff+=int((1u<<cat)-1u);
        uint value=(huff<<extra)|(uint(diff)&(extra==0u?0u:(1u<<extra)-1u));
        uint bits=hbits+extra, take=min(bits,32u-pending);
        uint high=value>>(bits-take);
        current|=(high&(take==32u?0xffffffffu:(1u<<take)-1u))<<(32u-pending-take);
        pending+=take; total+=bits;
        if(pending==32u) { packed.words[block*p.stride+word++]=current; current=0u; pending=0u; }
        if(bits>take) { uint tail=bits-take; current=(value&((1u<<tail)-1u))<<(32u-tail); pending=tail; }
    }
    if(pending!=0u)packed.words[block*p.stride+word]=current;
    lengths.bits[block]=total;
}
)glsl";
constexpr char scanShader[] = R"glsl(#version 450
layout(local_size_x=64) in;
layout(std430,binding=0) readonly buffer Lengths { uint bits[]; } lengths;
layout(std430,binding=1) writeonly buffer Offsets { uint bits[]; } offsets;
layout(std430,binding=2) writeonly buffer Sums { uint bits[]; } sums;
layout(push_constant) uniform Constants { uint blocks; uint groups; } p;
void main() {
    uint group=gl_GlobalInvocationID.x; if(group>=p.groups)return;
    uint begin=group*256u, end=min(begin+256u,p.blocks), sum=0u;
    for(uint i=begin;i<end;++i) { offsets.bits[i]=sum; sum+=lengths.bits[i]; }
    sums.bits[group]=sum;
}
)glsl";
constexpr char totalShader[] = R"glsl(#version 450
layout(local_size_x=1) in;
layout(std430,binding=0) readonly buffer Sums { uint bits[]; } sums;
layout(std430,binding=1) writeonly buffer Offsets { uint bits[]; } offsets;
layout(std430,binding=2) writeonly buffer Total { uint bits[]; } total;
layout(push_constant) uniform Constants { uint groups; } p;
void main() {
    uint sum=0u;
    for(uint i=0u;i<p.groups;++i) { offsets.bits[i]=sum; sum+=sums.bits[i]; }
    total.bits[0]=sum;
}
)glsl";
constexpr char clearShader[] = R"glsl(#version 450
layout(local_size_x=256) in;
layout(std430,binding=0) writeonly buffer Joined { uint words[]; } joined;
layout(std430,binding=1) writeonly buffer Output { uint words[]; } outputData;
layout(push_constant) uniform Constants { uint joinedWords; uint outputWords; } p;
void main() {
    uint first=gl_GlobalInvocationID.x*4u;
    for(uint i=0u;i<4u;++i) {
        uint word=first+i;
        if(word<p.joinedWords)joined.words[word]=0u;
        if(word<p.outputWords)outputData.words[word]=0u;
    }
}
)glsl";
constexpr char joinShader[] = R"glsl(#version 450
layout(local_size_x=64) in;
layout(std430,binding=0) readonly buffer Packed { uint words[]; } src;
layout(std430,binding=1) readonly buffer Lengths { uint bits[]; } lengths;
layout(std430,binding=2) readonly buffer Offsets { uint bits[]; } offsets;
layout(std430,binding=3) readonly buffer Groups { uint bits[]; } groups;
layout(std430,binding=4) buffer Joined { uint words[]; } dst;
layout(push_constant) uniform Constants { uint blocks; uint stride; uint capacityWords; } p;
void main() {
    uint block=gl_GlobalInvocationID.x>>3u, lane=gl_GlobalInvocationID.x&7u;
    if(block>=p.blocks)return;
    uint start=offsets.bits[block]+groups.bits[block>>8u], bits=lengths.bits[block];
    if(bits==0u)return;
    uint first=start>>5u, last=(start+bits-1u)>>5u;
    if(last>=p.capacityWords)return;
    uint shift=start&31u, rawWords=(bits+31u)>>5u;
    for(uint word=first+lane;word<=last;word+=8u) {
        uint j=word-first, right=j<rawWords?src.words[block*p.stride+j]:0u;
        uint value=right;
        if(shift!=0u) {
            uint left=j==0u?0u:src.words[block*p.stride+j-1u];
            value=(left<<(32u-shift))|(right>>shift);
        }
        bool boundary=(word==first&&shift!=0u)||
                      (word==last&&((start+bits)&31u)!=0u);
        if(boundary)atomicOr(dst.words[word],value); else dst.words[word]=value;
    }
}
)glsl";
constexpr char stuffCountShader[] = R"glsl(#version 450
layout(local_size_x=64) in;
layout(std430,binding=0) readonly buffer Joined { uint words[]; } src;
layout(std430,binding=1) readonly buffer Total { uint bits[]; } total;
layout(std430,binding=2) writeonly buffer WordOffsets { uint counts[]; } offsets;
layout(std430,binding=3) writeonly buffer GroupSums { uint counts[]; } sums;
layout(push_constant) uniform Constants { uint groups; uint joinedWords; } p;
shared uint laneSums[64];
void main() {
    uint group=gl_WorkGroupID.x; if(group>=p.groups)return;
    uint lane=gl_LocalInvocationID.x;
    uint rawBytes=(total.bits[0]+7u)>>3u;
    uint begin=group*256u+lane*4u, count[4], sum=0u;
    for(uint j=0u;j<4u;++j) {
        uint word=begin+j, start=word*4u;
        uint value=word<p.joinedWords && start<rawBytes?src.words[word]:0u;
        count[j]=0u;
        for(uint b=0u;b<4u;++b)
            if(start+b<rawBytes && ((value>>(24u-b*8u))&255u)==255u)count[j]++;
        sum+=count[j];
    }
    laneSums[lane]=sum;
    barrier();
    for(uint step=1u;step<64u;step<<=1u) {
        uint add=lane>=step?laneSums[lane-step]:0u;
        barrier();
        laneSums[lane]+=add;
        barrier();
    }
    uint prefix=laneSums[lane]-sum;
    for(uint j=0u;j<4u;++j) {
        uint word=begin+j;
        if(word<p.joinedWords)offsets.counts[word]=prefix;
        prefix+=count[j];
    }
    if(lane==63u)sums.counts[group]=laneSums[63];
}
)glsl";
constexpr char stuffTotalShader[] = R"glsl(#version 450
layout(local_size_x=256) in;
layout(std430,binding=0) readonly buffer GroupSums { uint counts[]; } sums;
layout(std430,binding=1) writeonly buffer GroupOffsets { uint counts[]; } offsets;
layout(std430,binding=2) readonly buffer Total { uint bits[]; } total;
layout(std430,binding=3) readonly buffer Table { uint values[]; } table;
layout(std430,binding=4) buffer Output { uint words[]; } outputData;
layout(push_constant) uniform Constants {
    uint groups; uint joinedWords; uint outputBytes;
    uint width; uint height; uint channels; uint bits; uint outputWords;
} p;
shared uint laneSums[256];
void put(uint offset,uint value) {
    atomicOr(outputData.words[offset>>2u],value<<(8u*(offset&3u)));
}
void emit(inout uint cursor,uint value) { put(cursor,value&255u); ++cursor; }
void emit16(inout uint cursor,uint value) { emit(cursor,value>>8u); emit(cursor,value); }
void main() {
    uint lane=gl_LocalInvocationID.x;
    uint groupsPerLane=(p.groups+255u)/256u;
    uint begin=lane*groupsPerLane, end=min(begin+groupsPerLane,p.groups);
    uint sum=0u;
    for(uint i=begin;i<end;++i) { offsets.counts[i]=sum; sum+=sums.counts[i]; }
    laneSums[lane]=sum;
    barrier();
    for(uint step=1u;step<256u;step<<=1u) {
        uint add=lane>=step?laneSums[lane-step]:0u;
        barrier();
        laneSums[lane]+=add;
        barrier();
    }
    uint groupBase=laneSums[lane]-sum;
    for(uint i=begin;i<end;++i)offsets.counts[i]+=groupBase;
    barrier();
    if(lane!=0u)return;
    sum=laneSums[255];
    uint rawBytes=(total.bits[0]+7u)>>3u, stuffed=rawBytes+sum;
    uint count=table.values[1], cursor=0u;
    emit(cursor,255u); emit(cursor,216u);
    emit(cursor,255u); emit(cursor,196u); emit16(cursor,19u+count); emit(cursor,0u);
    for(uint i=0u;i<16u;++i)emit(cursor,table.values[2u+i]);
    for(uint i=0u;i<count && i<17u;++i)emit(cursor,table.values[18u+i]);
    emit(cursor,255u); emit(cursor,195u); emit16(cursor,8u+3u*p.channels);
    emit(cursor,p.bits); emit16(cursor,p.height); emit16(cursor,p.width);
    emit(cursor,p.channels);
    for(uint i=0u;i<p.channels;++i) { emit(cursor,i); emit(cursor,17u); emit(cursor,0u); }
    emit(cursor,255u); emit(cursor,218u); emit16(cursor,6u+2u*p.channels);
    emit(cursor,p.channels);
    for(uint i=0u;i<p.channels;++i) { emit(cursor,i); emit(cursor,0u); }
    emit(cursor,table.values[0]); emit(cursor,0u); emit(cursor,0u);
    if(stuffed<=p.outputBytes) {
        put(cursor+stuffed,255u); put(cursor+stuffed+1u,217u);
    }
    outputData.words[p.outputWords]=uint(count>0u && count<=17u && total.bits[0]>0u &&
        ((total.bits[0]+31u)>>5u)<=p.joinedWords &&
        stuffed<=p.outputBytes ? cursor+stuffed+2u : 0u);
    outputData.words[p.outputWords+1u]=rawBytes;
    outputData.words[p.outputWords+2u]=stuffed;
    outputData.words[p.outputWords+3u]=cursor;
}
)glsl";
constexpr char stuffShader[] = R"glsl(#version 450
layout(local_size_x=64) in;
layout(std430,binding=0) readonly buffer Joined { uint words[]; } src;
layout(std430,binding=1) readonly buffer Total { uint bits[]; } total;
layout(std430,binding=2) readonly buffer WordOffsets { uint counts[]; } offsets;
layout(std430,binding=3) readonly buffer GroupOffsets { uint counts[]; } groups;
layout(std430,binding=4) buffer Output { uint words[]; } dst;
layout(push_constant) uniform Constants { uint joinedWords; uint outputBytes; uint outputWords; } p;
void writeByte(uint offset,uint value) {
    if(offset>=p.outputBytes)return;
    atomicOr(dst.words[offset>>2u],value<<(8u*(offset&3u)));
}
void main() {
    uint word=gl_GlobalInvocationID.x, rawBytes=(total.bits[0]+7u)>>3u;
    if(word>=p.joinedWords || word*4u>=rawBytes)return;
    uint value=src.words[word];
    uint offset=dst.words[p.outputWords+3u]+word*4u+offsets.counts[word]+groups.counts[word>>8u];
    for(uint b=0u;b<4u && word*4u+b<rawBytes;++b) {
        uint byte=(value>>(24u-b*8u))&255u;
        writeByte(offset++,byte);
        if(byte==255u)writeByte(offset++,0u);
    }
}
)glsl";
enum Buffer : int {
  Src,
  Upload,
  Histogram,
  HistogramZero,
  Table,
  Packed,
  Lengths,
  Offsets,
  Sums,
  GroupOffsets,
  Total,
  Joined,
  WordOffsets,
  StuffSums,
  StuffOffsets,
  Output,
  OutputStage,
  BufferCount
};
enum Pass : int {
  HistogramPass,
  TablePass,
  PackPass,
  ScanPass,
  TotalPass,
  ClearPass,
  JoinPass,
  StuffCountPass,
  StuffTotalPass,
  StuffPass,
  HistogramTexturePass,
  PackTexturePass,
  HistogramRgbTexturePass,
  PackRgbTexturePass,
  PassCount
};
pl_pass makePass(pl_gpu gpu, const char *source,
                 std::initializer_list<pl_desc> descriptors, int constants) {
  std::vector<pl_desc> list(descriptors);
  pl_pass_params params{};
  params.type = PL_PASS_COMPUTE;
  params.glsl_shader = source;
  params.descriptors = list.data();
  params.num_descriptors = int(list.size());
  params.push_constants_size = constants * sizeof(uint32_t);
  return pl_pass_create(gpu, &params);
}
std::string textureShader(const char *source, bool rgb) {
  std::string shader(source);
  shader.insert(shader.find('\n') + 1u,
                rgb ? "#define JPEG_SRC_TEXTURE\n#define JPEG_SRC_RGB\n"
                    : "#define JPEG_SRC_TEXTURE\n");
  return shader;
}
struct State {
  pl_gpu gpu = nullptr;
  pl_buf b[BufferCount]{};
  pl_pass p[PassCount]{};
  size_t lastRawBytes = 0, lastStuffedBytes = 0;
  uint32_t lastWidth = 0, lastHeight = 0, lastChannels = 0, lastBits = 0;
  ~State() {
    if (!gpu)
      return;
    pl_gpu_finish(gpu);
    for (auto &x : b)
      pl_buf_destroy(gpu, &x);
    for (auto &x : p)
      pl_pass_destroy(gpu, &x);
  }
  bool init(pl_gpu sharedGpu) {
    if (p[0])
      return std::all_of(std::begin(p), std::begin(p) + StuffPass + 1,
                         [](pl_pass pass) { return pass != nullptr; });
    gpu = sharedGpu;
    p[HistogramPass] =
        makePass(gpu, histogramShader,
                 {{"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
                  {"hist", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READWRITE}},
                 6);
    p[TablePass] = makePass(
        gpu, tableShader,
        {{"hist", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
         {"outTable", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY}},
        1);
    p[PackPass] = makePass(
        gpu, packShader,
        {{"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
         {"table", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
         {"packed", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY},
         {"lengths", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_WRITEONLY}},
        8);
    p[ScanPass] =
        makePass(gpu, scanShader,
                 {{"lengths", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
                  {"offsets", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY},
                  {"sums", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY}},
                 2);
    p[TotalPass] =
        makePass(gpu, totalShader,
                 {{"sums", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
                  {"offsets", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY},
                  {"total", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY}},
                 1);
    p[ClearPass] = makePass(
        gpu, clearShader,
        {{"joined", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_WRITEONLY},
         {"outputData", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY}},
        2);
    p[JoinPass] =
        makePass(gpu, joinShader,
                 {{"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
                  {"lengths", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
                  {"offsets", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_READONLY},
                  {"groups", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY},
                  {"dst", PL_DESC_BUF_STORAGE, 4, PL_DESC_ACCESS_READWRITE}},
                 3);
    p[StuffCountPass] =
        makePass(gpu, stuffCountShader,
                 {{"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
                  {"total", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
                  {"offsets", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY},
                  {"sums", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_WRITEONLY}},
                 2);
    p[StuffTotalPass] = makePass(
        gpu, stuffTotalShader,
        {{"sums", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
         {"offsets", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY},
         {"total", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_READONLY},
         {"table", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY},
         {"outputData", PL_DESC_BUF_STORAGE, 4, PL_DESC_ACCESS_READWRITE}},
        8);
    p[StuffPass] =
        makePass(gpu, stuffShader,
                 {{"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
                  {"total", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
                  {"offsets", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_READONLY},
                  {"groups", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY},
                  {"dst", PL_DESC_BUF_STORAGE, 4, PL_DESC_ACCESS_READWRITE}},
                 3);
    for (int i = HistogramPass; i <= StuffPass; ++i)
      if (!p[i])
        return false;
    return true;
  }
  bool initTexture(pl_gpu sharedGpu, bool rgb) {
    if (!init(sharedGpu))
      return false;
    const Pass histogramPass = rgb ? HistogramRgbTexturePass : HistogramTexturePass;
    const Pass packPass = rgb ? PackRgbTexturePass : PackTexturePass;
    if (!p[histogramPass]) {
      const auto shader = textureShader(histogramShader, rgb);
      p[histogramPass] = makePass(
          gpu, shader.c_str(),
          {{"srcTex", PL_DESC_SAMPLED_TEX, 0, PL_DESC_ACCESS_READONLY},
           {"hist", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READWRITE}},
          6);
    }
    if (!p[packPass]) {
      const auto shader = textureShader(packShader, rgb);
      p[packPass] = makePass(
          gpu, shader.c_str(),
          {{"srcTex", PL_DESC_SAMPLED_TEX, 0, PL_DESC_ACCESS_READONLY},
           {"table", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
           {"packed", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY},
           {"lengths", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_WRITEONLY}},
          8);
    }
    return p[histogramPass] && p[packPass];
  }
  bool buffer(Buffer index, size_t bytes, bool hostReadable = false,
              bool hostWritable = false, bool hostMapped = false) {
    pl_buf_params params{};
    params.size = bytes;
    params.storable = !hostReadable && !hostWritable;
    params.host_readable = hostReadable;
    params.host_writable = hostWritable;
    params.host_mapped = hostMapped;
    params.memory_type =
        hostReadable || hostWritable ? PL_BUF_MEM_HOST : PL_BUF_MEM_DEVICE;
    return bytes > 0 && bytes <= gpu->limits.max_ssbo_size &&
           pl_buf_recreate(gpu, &b[index], &params);
  }
  void run(Pass pass, std::initializer_list<Buffer> objects, void *constants,
           int groups) {
    std::vector<pl_desc_binding> bindings(objects.size());
    int i = 0;
    for (auto object : objects)
      bindings[i++].object = b[object];
    pl_pass_run_params run{};
    run.pass = p[pass];
    run.desc_bindings = bindings.data();
    run.push_constants = constants;
    run.compute_groups[0] = groups;
    run.compute_groups[1] = run.compute_groups[2] = 1;
    pl_pass_run(gpu, &run);
  }
  void runTexture(Pass pass, pl_tex texture,
                  std::initializer_list<Buffer> objects, void *constants,
                  int groups) {
    std::vector<pl_desc_binding> bindings(objects.size() + 1u);
    bindings[0].object = texture;
    int i = 1;
    for (auto object : objects)
      bindings[i++].object = b[object];
    pl_pass_run_params run{};
    run.pass = p[pass];
    run.desc_bindings = bindings.data();
    run.push_constants = constants;
    run.compute_groups[0] = groups;
    run.compute_groups[1] = run.compute_groups[2] = 1;
    pl_pass_run(gpu, &run);
  }
};
State &state() {
  static State value;
  return value;
}

struct ExternalStates {
  std::mutex mutex;
  std::unordered_map<pl_gpu, std::unique_ptr<State>> states;
};
ExternalStates &externalStates() {
  static ExternalStates value;
  return value;
}

bool encodeOnGpu(pl_gpu gpu, State &s, const uint16_t *pixels, pl_tex texture,
                 pl_buf packedInput, uint32_t packedWordsPerRow,
                 uint32_t width, uint32_t height, uint32_t channels,
                 uint32_t bits, uint8_t **encoded, int *encodedLength,
                 const JpegOutputConsumer *consume = nullptr) {
  if ((!pixels && !texture && !packedInput) || !encoded || !encodedLength ||
      !width || !height ||
      width > 65535u || height > 65535u || (channels != 1u && channels != 3u) ||
      bits < 8u || bits > 16u)
    return false;
  *encoded = nullptr;
  *encodedLength = 0;
  const uint64_t count64 = uint64_t(width) * height * channels;
  if (count64 < 1000000u || count64 > INT_MAX || count64 > UINT32_MAX / 32u)
    return false;
  const uint32_t samples = uint32_t(count64);
  const uint32_t blocks = (samples + kBlock - 1u) / kBlock;
  const uint32_t groups = (blocks + 255u) / 256u;
  const size_t maxRaw = uint64_t(samples) * 4u;
  const bool directTexture = texture && texture->params.sampleable;
  if (!(directTexture ? s.initTexture(gpu, channels == 3u) : s.init(gpu)))
    return false;
  const bool stageProfile =
      std::getenv("MOTIONCAM_JPEG_STAGE_PROFILE") != nullptr;
  // RGB's larger reserved payload is cheaper to read through the completed
  // mapped staging buffer and copy at the exact JPEG length. CFA is faster
  // through pl_buf_read on the measured Vulkan device.
  const bool mappedReadback = channels == 3u || consume != nullptr;
  auto stageStart = std::chrono::steady_clock::now();
  auto markStage = [&](const char *name) {
    if (!stageProfile)
      return;
    pl_gpu_finish(gpu);
    const auto now = std::chrono::steady_clock::now();
    std::fprintf(
        stderr, "JPEG GPU stage %s %.3f ms\n", name,
        std::chrono::duration<double, std::milli>(now - stageStart).count());
    stageStart = now;
  };
  const bool reuse = s.lastWidth == width && s.lastHeight == height &&
                     s.lastChannels == channels && s.lastBits == bits;
  const size_t joinedBytes =
      std::min(maxRaw, reuse && s.lastRawBytes
                           ? s.lastRawBytes + s.lastRawBytes / 8u + 1048576u
                           : size_t(samples) * 3u);
  const uint32_t joinedWords = uint32_t((joinedBytes + 3u) / 4u);
  const uint32_t stuffGroups = (joinedWords + 255u) / 256u;
  const size_t maxOutput = maxRaw * 2u;
  const size_t outputBytes = std::min(
      maxOutput, reuse && s.lastStuffedBytes
                     ? s.lastStuffedBytes + s.lastStuffedBytes / 8u + 1048576u
                     : size_t(joinedWords) * 8u);
  const size_t payloadCapacity = outputBytes + 128u;
  const size_t outputWords = (payloadCapacity + 3u) / 4u;
  // Keep the JPEG at offset zero so the final readback can fill the returned
  // allocation directly. The four metadata words follow the padded payload.
  const size_t outputBufferBytes = (outputWords + kMetaWords) * 4u;
  const size_t sourceBytes = packedInput
      ? size_t(packedWordsPerRow) * height * 4u
      : (size_t(samples) + 1u) / 2u * 4u;
  if (sourceBytes > UINT32_MAX)
    return false;
  if (packedInput && (!packedWordsPerRow ||
      packedWordsPerRow < (uint64_t(width) * channels * bits + 31u) / 32u ||
      sourceBytes > packedInput->params.size))
    return false;
  const size_t histBytes = 8u * 18u * 4u;
  const size_t packedBytes = size_t(blocks) * kBlock * 4u;
  if (outputBufferBytes > gpu->limits.max_ssbo_size ||
      packedBytes > gpu->limits.max_ssbo_size ||
      (samples + 255u) / 256u > uint32_t(gpu->limits.max_dispatch[0]) ||
      (uint64_t(blocks) + 15u) / 16u >
          uint64_t(gpu->limits.max_dispatch[0]) ||
      (std::max<size_t>(joinedWords, outputBufferBytes / 4u) + 1023u) / 1024u >
          uint32_t(gpu->limits.max_dispatch[0]) ||
      (joinedWords + 63u) / 64u > uint32_t(gpu->limits.max_dispatch[0]) ||
      (uint64_t(blocks) * 8u + 63u) / 64u >
          uint64_t(gpu->limits.max_dispatch[0]) ||
      stuffGroups > uint32_t(gpu->limits.max_dispatch[0])) {
    if (std::getenv("MOTIONCAM_JPEG_ENCODE_PROFILE"))
      std::fprintf(stderr,
                   "JPEG GPU limits: samples=%u packed=%zu output=%zu "
                   "max_ssbo=%zu max_dispatch=%d\n",
                   samples, packedBytes, outputBufferBytes,
                   size_t(gpu->limits.max_ssbo_size),
                   gpu->limits.max_dispatch[0]);
    return false;
  }
  const struct {
    Buffer index;
    size_t size;
    bool readable;
    bool writable;
  } buffers[] = {{Src, sourceBytes, false, false},
                 {Upload, sourceBytes, false, true},
                 {Histogram, histBytes, false, false},
                 {HistogramZero, histBytes, false, true},
                 {Table, 69u * 4u, false, false},
                 {Packed, packedBytes, false, false},
                 {Lengths, size_t(blocks) * 4u, false, false},
                 {Offsets, size_t(blocks) * 4u, false, false},
                 {Sums, size_t(groups) * 4u, false, false},
                 {GroupOffsets, size_t(groups) * 4u, false, false},
                 {Total, 4u, false, false},
                 {Joined, size_t(joinedWords) * 4u, false, false},
                 {WordOffsets, size_t(joinedWords) * 4u, false, false},
                 {StuffSums, size_t(stuffGroups) * 4u, false, false},
                 {StuffOffsets, size_t(stuffGroups) * 4u, false, false},
                 {Output, outputBufferBytes, false, false},
                 {OutputStage, outputBufferBytes, true, false}};
  if (s.b[OutputStage] &&
      s.b[OutputStage]->params.host_mapped != mappedReadback)
    pl_buf_destroy(gpu, &s.b[OutputStage]);
  for (const auto &item : buffers)
    if (!s.buffer(item.index, item.size, item.readable, item.writable,
                  item.index == OutputStage && mappedReadback)) {
      if (std::getenv("MOTIONCAM_JPEG_ENCODE_PROFILE"))
        std::fprintf(stderr, "JPEG GPU buffer failed: index=%d bytes=%zu\n",
                     item.index, item.size);
      return false;
    }
  if (packedInput) {
    pl_buf_copy(gpu, s.b[Src], 0, packedInput, 0, sourceBytes);
  } else if (texture && !directTexture) {
    pl_tex_transfer_params transfer{};
    transfer.tex = texture;
    transfer.buf = s.b[Src];
    transfer.row_pitch = size_t(width) * sizeof(uint16_t);
    if (!pl_tex_download(gpu, &transfer))
      return false;
  } else if (pixels) {
    pl_buf_write(gpu, s.b[Upload], 0, pixels, size_t(samples) * 2u);
    if (samples & 1u) {
      const uint16_t zero = 0;
      pl_buf_write(gpu, s.b[Upload], size_t(samples) * 2u, &zero, 2u);
    }
    pl_buf_copy(gpu, s.b[Src], 0, s.b[Upload], 0, sourceBytes);
  }
  std::array<uint32_t, 8u * 18u> emptyHistogram{};
  pl_buf_write(gpu, s.b[HistogramZero], 0, emptyHistogram.data(), histBytes);
  pl_buf_copy(gpu, s.b[Histogram], 0, s.b[HistogramZero], 0, histBytes);
  markStage("upload");
  uint32_t dimensions[6] = {width, height, channels, bits,
                            packedInput ? bits : 16u,
                            packedInput ? packedWordsPerRow : 0u};
  const uint32_t histogramInvocations =
      directTexture && channels == 3u
          ? (samples >= 1000000u
                 ? height * ((width + 15u) / 16u) * 2u
                 : width * height)
          : samples;
  if (directTexture)
    s.runTexture(channels == 3u ? HistogramRgbTexturePass
                                : HistogramTexturePass,
                 texture, {Histogram}, dimensions,
                 int((histogramInvocations + 255u) / 256u));
  else
    s.run(HistogramPass, {Src, Histogram}, dimensions,
          int((histogramInvocations + 255u) / 256u));
  markStage("histogram");
  uint32_t sampleCount = samples;
  s.run(TablePass, {Histogram, Table}, &sampleCount, 1);
  markStage("table");
  uint32_t packConstants[8] = {samples, kBlock, width, height, channels,
                               bits, packedInput ? bits : 16u,
                               packedInput ? packedWordsPerRow : 0u};
  if (directTexture)
    s.runTexture(channels == 3u ? PackRgbTexturePass : PackTexturePass,
                 texture, {Table, Packed, Lengths},
                 packConstants, int((blocks + 15u) / 16u));
  else
    s.run(PackPass, {Src, Table, Packed, Lengths}, packConstants,
          int((blocks + 15u) / 16u));
  markStage("pack");
  uint32_t scanConstants[2] = {blocks, groups};
  s.run(ScanPass, {Lengths, Offsets, Sums}, scanConstants,
        int((groups + 63u) / 64u));
  s.run(TotalPass, {Sums, GroupOffsets, Total}, &scanConstants[1], 1);
  markStage("prefix");
  uint32_t clearConstants[2] = {joinedWords, uint32_t(outputBufferBytes / 4u)};
  s.run(ClearPass, {Joined, Output}, clearConstants,
        int((std::max(clearConstants[0], clearConstants[1]) + 1023u) / 1024u));
  markStage("clear");
  uint32_t joinConstants[3] = {blocks, kBlock, joinedWords};
  s.run(JoinPass, {Packed, Lengths, Offsets, GroupOffsets, Joined},
        joinConstants, int((uint64_t(blocks) * 8u + 63u) / 64u));
  markStage("join");
  uint32_t countConstants[2] = {stuffGroups, joinedWords};
  s.run(StuffCountPass, {Joined, Total, WordOffsets, StuffSums}, countConstants,
        int(stuffGroups));
  markStage("count_stuff");
  uint32_t totalConstants[8] = {stuffGroups, joinedWords, uint32_t(outputBytes),
                                width,       height,      channels,
                                bits,        uint32_t(outputWords)};
  s.run(StuffTotalPass, {StuffSums, StuffOffsets, Total, Table, Output},
        totalConstants, 1);
  markStage("stuff_prefix_header");
  uint32_t stuffConstants[3] = {joinedWords, uint32_t(payloadCapacity),
                                uint32_t(outputWords)};
  s.run(StuffPass, {Joined, Total, WordOffsets, StuffOffsets, Output},
        stuffConstants, int((joinedWords + 63u) / 64u));
  markStage("stuff");
  pl_buf_copy(gpu, s.b[OutputStage], 0, s.b[Output], 0, outputBufferBytes);
  markStage("output_copy");
  uint8_t *readback = nullptr;
  if (mappedReadback) {
    if (!s.b[OutputStage]->data)
      return false;
    while (pl_buf_poll(gpu, s.b[OutputStage], UINT64_MAX)) {}
    readback = s.b[OutputStage]->data;
    markStage("output_wait");
  } else {
    readback = static_cast<uint8_t *>(std::malloc(outputBufferBytes));
    if (!readback)
      return false;
    if (!pl_buf_read(gpu, s.b[OutputStage], 0, readback,
                     outputBufferBytes)) {
      std::free(readback);
      return false;
    }
    markStage("readback");
  }
  const uint32_t *metadata =
      reinterpret_cast<const uint32_t *>(readback) + outputWords;
  if (std::getenv("MOTIONCAM_JPEG_ENCODE_PROFILE"))
    std::fprintf(stderr,
                 "JPEG full GPU: jpeg=%u raw=%u stuffed=%u header=%u "
                 "joined_capacity=%u output_capacity=%zu\n",
                 metadata[0], metadata[1], metadata[2], metadata[3],
                 joinedWords, outputBytes);
  const uint32_t jpegBytes = metadata[0];
  if (!jpegBytes || jpegBytes > payloadCapacity || jpegBytes > INT32_MAX ||
      metadata[1] > joinedBytes || metadata[2] > outputBytes ||
      metadata[3] < 40u || metadata[3] > 128u ||
      uint64_t(metadata[3]) + metadata[2] + 2u != jpegBytes) {
    if (!mappedReadback)
      std::free(readback);
    return false;
  }
  if (consume) {
    if (!(*consume)(readback, jpegBytes))
      return false;
    markStage("consume");
  } else {
    auto *result = readback;
    if (mappedReadback) {
      result = static_cast<uint8_t *>(std::malloc(jpegBytes));
      if (!result)
        return false;
      std::memcpy(result, readback, jpegBytes);
      markStage("host_copy");
    }
    *encoded = result;
  }
  *encodedLength = int(jpegBytes);
  s.lastRawBytes = reuse ? std::max(s.lastRawBytes, size_t(metadata[1]))
                         : size_t(metadata[1]);
  s.lastStuffedBytes = reuse ? std::max(s.lastStuffedBytes, size_t(metadata[2]))
                             : size_t(metadata[2]);
  s.lastWidth = width;
  s.lastHeight = height;
  s.lastChannels = channels;
  s.lastBits = bits;
  return true;
}
} // namespace

bool jpegEncodeFullyVulkan(const uint16_t *pixels, uint32_t width,
                           uint32_t height, uint32_t channels, uint32_t bits,
                           uint8_t **encoded, int *encodedLength) {
  auto &context = jpegContext();
  std::lock_guard lock(context.mutex);
  if (!context.init())
    return false;
  return encodeOnGpu(context.vulkan->gpu, state(), pixels, nullptr, nullptr, 0,
                     width,
                     height, channels, bits, encoded, encodedLength);
}

bool jpegEncodeFullyVulkanBorrowed(const uint16_t *pixels, uint32_t width,
                                   uint32_t height, uint32_t channels,
                                   uint32_t bits,
                                   const JpegOutputConsumer &consume) {
  if (!consume)
    return false;
  auto &context = jpegContext();
  std::lock_guard lock(context.mutex);
  if (!context.init())
    return false;
  uint8_t *unused = nullptr;
  int length = 0;
  return encodeOnGpu(context.vulkan->gpu, state(), pixels, nullptr, nullptr, 0,
                     width,
                     height, channels, bits, &unused, &length, &consume);
}

bool jpegEncodeFullyVulkanTexture(const void *gpuHandle,
                                  const void *textureHandle, uint32_t width,
                                  uint32_t height, uint8_t **encoded,
                                  int *encodedLength) {
  return jpegEncodeFullyVulkanTexture(gpuHandle, textureHandle, width, height,
                                      1, 16, encoded, encodedLength);
}

static bool encodeTextureOnGpu(const void *gpuHandle,
                               const void *textureHandle, uint32_t width,
                               uint32_t height, uint32_t channels,
                               uint32_t bits, uint8_t **encoded,
                               int *encodedLength,
                               const JpegOutputConsumer *consume) {
  auto gpu = static_cast<pl_gpu>(const_cast<void *>(gpuHandle));
  auto texture = static_cast<pl_tex>(const_cast<void *>(textureHandle));
  if (!gpu || !texture || !texture->params.format ||
      texture->params.w != int(width) || texture->params.h != int(height) ||
      (channels != 1u && channels != 3u) || bits < 8u || bits > 16u ||
      (!texture->params.sampleable &&
       (channels != 1u || !texture->params.host_readable ||
        !gpu->limits.buf_transfer)))
    return false;
  const auto format = texture->params.format;
  if (format->opaque || (channels == 1u && format->emulated) ||
      format->type != PL_FMT_UNORM ||
      (channels == 1u ? format->num_components != 1
                      : format->num_components < 3) ||
      (channels == 1u ? format->texel_size != sizeof(uint16_t)
                      : format->texel_size < 3u * sizeof(uint16_t))) {
    if (std::getenv("MOTIONCAM_JPEG_ENCODE_PROFILE"))
      std::fprintf(stderr,
                   "JPEG GPU texture format rejected: type=%d components=%d "
                   "texel_size=%zu opaque=%d emulated=%d\n",
                   format->type, format->num_components, format->texel_size,
                   format->opaque, format->emulated);
    return false;
  }
  for (uint32_t component = 0; component < channels; ++component)
    if (format->host_bits[component] != 16 ||
        format->sample_order[component] != int(component)) {
      if (std::getenv("MOTIONCAM_JPEG_ENCODE_PROFILE"))
        std::fprintf(stderr,
                     "JPEG GPU texture component rejected: index=%u "
                     "bits=%d order=%d\n",
                     component, format->host_bits[component],
                     format->sample_order[component]);
      return false;
    }
  auto &external = externalStates();
  std::lock_guard lock(external.mutex);
  auto &slot = external.states[gpu];
  if (!slot)
    slot = std::make_unique<State>();
  return encodeOnGpu(gpu, *slot, nullptr, texture, nullptr, 0,
                     width, height, channels, bits,
                     encoded, encodedLength, consume);
}

bool jpegEncodeFullyVulkanPackedBufferBorrowed(
    const void *gpuHandle, const void *bufferHandle, uint32_t width,
    uint32_t height, uint32_t channels, uint32_t bits,
    uint32_t wordsPerRow, const JpegOutputConsumer &consume) {
  auto gpu = static_cast<pl_gpu>(const_cast<void *>(gpuHandle));
  auto buffer = static_cast<pl_buf>(const_cast<void *>(bufferHandle));
  if (!gpu || !buffer || !consume || !buffer->params.storable ||
      (channels != 1u && channels != 3u) || bits < 8u || bits > 16u)
    return false;
  auto &external = externalStates();
  std::lock_guard lock(external.mutex);
  auto &slot = external.states[gpu];
  if (!slot)
    slot = std::make_unique<State>();
  uint8_t *unused = nullptr;
  int length = 0;
  return encodeOnGpu(gpu, *slot, nullptr, nullptr, buffer, wordsPerRow,
                     width, height, channels, bits, &unused, &length, &consume);
}

bool jpegEncodeFullyVulkanTexture(const void *gpuHandle,
                                  const void *textureHandle, uint32_t width,
                                  uint32_t height, uint32_t channels,
                                  uint32_t bits, uint8_t **encoded,
                                  int *encodedLength) {
  return encodeTextureOnGpu(gpuHandle, textureHandle, width, height, channels,
                            bits, encoded, encodedLength, nullptr);
}

bool jpegEncodeFullyVulkanTextureBorrowed(
    const void *gpuHandle, const void *textureHandle, uint32_t width,
    uint32_t height, uint32_t channels, uint32_t bits,
    const JpegOutputConsumer &consume) {
  if (!consume)
    return false;
  uint8_t *unused = nullptr;
  int length = 0;
  return encodeTextureOnGpu(gpuHandle, textureHandle, width, height, channels,
                            bits, &unused, &length, &consume);
}

void releaseVulkanJpegFullEncodeDevice(const void *gpuHandle) {
  auto &external = externalStates();
  std::lock_guard lock(external.mutex);
  external.states.erase(
      static_cast<pl_gpu>(const_cast<void *>(gpuHandle)));
}
} // namespace motioncam
#else
namespace motioncam {
bool jpegEncodeFullyVulkan(const uint16_t *, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint8_t **, int *) {
  return false;
}
bool jpegEncodeFullyVulkanBorrowed(const uint16_t *, uint32_t, uint32_t,
                                   uint32_t, uint32_t,
                                   const JpegOutputConsumer &) {
  return false;
}
bool jpegEncodeFullyVulkanTexture(const void *, const void *, uint32_t,
                                  uint32_t, uint8_t **, int *) {
  return false;
}
bool jpegEncodeFullyVulkanTexture(const void *, const void *, uint32_t,
                                  uint32_t, uint32_t, uint32_t,
                                  uint8_t **, int *) {
  return false;
}
bool jpegEncodeFullyVulkanTextureBorrowed(
    const void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t,
    const JpegOutputConsumer &) {
  return false;
}
bool jpegEncodeFullyVulkanPackedBufferBorrowed(
    const void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, const JpegOutputConsumer &) {
  return false;
}
void releaseVulkanJpegFullEncodeDevice(const void *) {}
} // namespace motioncam
#endif
