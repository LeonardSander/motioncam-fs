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
        int range=65536;
#ifdef JPEG_TILED_HISTOGRAM
        range=int(1u<<p.bits);
#endif
        int d=(v-pred)&(range-1); if(d>=range/2) d-=range;
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
constexpr char tiledTextureShader[] = R"glsl(#version 450
layout(local_size_x = 8) in;
#ifdef JPEG_SRC_BUFFER
layout(std430, binding = 0) readonly buffer Source { uint words[]; } src;
#else
layout(binding = 0) uniform sampler2D srcTex;
#endif
layout(std430, binding = 1) writeonly buffer Bodies { uint words[]; } bodies;
layout(std430, binding = 2) writeonly buffer Lengths { uint words[]; } lengths;
layout(std430, binding = 3) readonly buffer Table { uint values[]; } table;
layout(push_constant) uniform Constants {
    uint width; uint height; uint tileWidth; uint tileHeight;
    uint channels; uint strideWords; uint tiles; uint sourceWordsPerRow;
    uint sourceBits;
} p;
void emitByte(uint tile, uint value, inout uint bytePos,
              inout uint word, inout bool good) {
    if (bytePos >= p.strideWords * 4u) { good = false; return; }
    word |= value << ((bytePos & 3u) * 8u);
    ++bytePos;
    if ((bytePos & 3u) == 0u) {
        bodies.words[tile * p.strideWords + (bytePos >> 2u) - 1u] = word;
        word = 0u;
    }
}
#ifdef JPEG_DIRECT_SLOT
void emit16(uint tile, uint value, inout uint bytePos,
            inout uint word, inout bool good) {
    emitByte(tile, value >> 8u, bytePos, word, good);
    emitByte(tile, value, bytePos, word, good);
}
void emitHeader(uint tile, inout uint bytePos,
                inout uint word, inout bool good) {
    emitByte(tile, 255u, bytePos, word, good);
    emitByte(tile, 216u, bytePos, word, good);
    emitByte(tile, 255u, bytePos, word, good);
    emitByte(tile, 196u, bytePos, word, good);
    emit16(tile, 19u + table.values[1], bytePos, word, good);
    emitByte(tile, 0u, bytePos, word, good);
    for (uint i = 0u; i < 16u; ++i)
        emitByte(tile, table.values[2u + i], bytePos, word, good);
    for (uint i = 0u; i < table.values[1]; ++i)
        emitByte(tile, table.values[18u + i], bytePos, word, good);
    emitByte(tile, 255u, bytePos, word, good);
    emitByte(tile, 195u, bytePos, word, good);
    emit16(tile, 8u + 3u * p.channels, bytePos, word, good);
    emitByte(tile, p.sourceBits, bytePos, word, good);
    emit16(tile, p.tileHeight, bytePos, word, good);
    emit16(tile, p.tileWidth, bytePos, word, good);
    emitByte(tile, p.channels, bytePos, word, good);
    for (uint c = 0u; c < p.channels; ++c) {
        emitByte(tile, c, bytePos, word, good);
        emitByte(tile, 17u, bytePos, word, good);
        emitByte(tile, 0u, bytePos, word, good);
    }
    emitByte(tile, 255u, bytePos, word, good);
    emitByte(tile, 218u, bytePos, word, good);
    emit16(tile, 6u + 2u * p.channels, bytePos, word, good);
    emitByte(tile, p.channels, bytePos, word, good);
    for (uint c = 0u; c < p.channels; ++c) {
        emitByte(tile, c, bytePos, word, good);
        emitByte(tile, 0u, bytePos, word, good);
    }
    emitByte(tile, table.values[0], bytePos, word, good);
    emitByte(tile, 0u, bytePos, word, good);
    emitByte(tile, 0u, bytePos, word, good);
}
#endif
void appendBits(uint tile, uint value, uint count,
                inout uint reservoir, inout uint pending,
                inout uint bytePos, inout uint word, inout bool good) {
    reservoir = (reservoir << count) | value;
    pending += count;
    while (pending >= 8u && good) {
        uint b = (reservoir >> (pending - 8u)) & 255u;
        emitByte(tile, b, bytePos, word, good);
        if (b == 255u && good) emitByte(tile, 0u, bytePos, word, good);
        pending -= 8u;
        reservoir &= (1u << pending) - 1u;
    }
}
int predictTile(uint mode, int a, int b, int c) {
    if (mode == 1u) return a;
    if (mode == 2u) return b;
    if (mode == 3u) return c;
    if (mode == 4u) return a + b - c;
    if (mode == 5u) return a + ((b - c) >> 1);
    if (mode == 6u) return b + ((a - c) >> 1);
    return (a + b) >> 1;
}
vec4 loadPixel(ivec2 pos) {
#ifdef JPEG_SRC_BUFFER
    vec4 values = vec4(0.0);
    for (uint c = 0u; c < p.channels; ++c) {
        uint i = uint(pos.x) * p.channels + c;
        if (p.sourceBits == 16u) {
            uint word = src.words[uint(pos.y) * p.sourceWordsPerRow + (i >> 1u)];
            values[c] = float((i & 1u) == 0u ? word & 65535u : word >> 16u);
            continue;
        }
        uint bit = i * p.sourceBits;
        uint byteIndex = uint(pos.y) * p.sourceWordsPerRow * 4u + (bit >> 3u);
        uint byteInRow = bit >> 3u;
        uint rowBytes = p.sourceWordsPerRow * 4u;
        uint first = src.words[byteIndex >> 2u] >> ((byteIndex & 3u) * 8u);
        uint second = byteInRow + 1u < rowBytes ?
            src.words[(byteIndex + 1u) >> 2u] >> (((byteIndex + 1u) & 3u) * 8u) : 0u;
        uint third = byteInRow + 2u < rowBytes ?
            src.words[(byteIndex + 2u) >> 2u] >> (((byteIndex + 2u) & 3u) * 8u) : 0u;
        uint window = ((first & 255u) << 16u) | ((second & 255u) << 8u) |
                      (third & 255u);
        values[c] = float((window >> (24u - p.sourceBits - (bit & 7u))) &
                          ((1u << p.sourceBits) - 1u));
    }
    return values;
#else
    return texelFetch(srcTex, pos, 0) * 65535.0;
#endif
}
void main() {
    uint tile = gl_GlobalInvocationID.x;
    if (tile >= p.tiles) return;
    uint across = p.width / p.tileWidth;
    uint ox = (tile % across) * p.tileWidth;
    uint oy = (tile / across) * p.tileHeight;
    uint bytePos = 0u, word = 0u, reservoir = 0u, pending = 0u;
    bool good = table.values[1] > 0u;
    uint mode = table.values[0];
    uint bodyStart = 0u;
#ifdef JPEG_DIRECT_SLOT
    emitHeader(tile, bytePos, word, good);
    bodyStart = bytePos;
#endif
    for (uint y = 0u; y < p.tileHeight && good; ++y) {
        vec4 previous = vec4(0.0);
        vec4 previousAbove = vec4(0.0);
        for (uint x = 0u; x < p.tileWidth && good; ++x) {
            ivec2 pos = ivec2(int(ox + x), int(oy + y));
            vec4 v = loadPixel(pos);
            vec4 a = previous;
            vec4 b = y > 0u ? loadPixel(pos + ivec2(0, -1)) : vec4(0.0);
            vec4 c = previousAbove;
            for (uint component = 0u; component < p.channels; ++component) {
                int sampleValue = int(round(v[component]));
                int left = int(round(a[component]));
                int above = int(round(b[component]));
                int diagonal = int(round(c[component]));
                int pred = y == 0u && x == 0u ? int(1u << (p.sourceBits - 1u)) :
                           y == 0u ? left : x == 0u ? above :
                           predictTile(mode, left, above, diagonal);
                int range = int(1u << p.sourceBits);
                int diff = (sampleValue - pred) & (range - 1);
                if (diff >= range / 2) diff -= range;
                uint mag = uint(abs(diff));
                uint category = mag == 0u ? 0u : uint(findMSB(mag) + 1);
                uint extra = category == 16u ? 0u : category;
                if (diff < int(category == 0u ? 0u : 1u << (category - 1u)))
                    diff += int((1u << category) - 1u);
                uint huff = table.values[35u + category];
                uint huffBits = table.values[52u + category];
                uint difference = uint(diff) &
                    (extra == 0u ? 0u : (1u << extra) - 1u);
                if (huffBits + extra <= 24u) {
                    appendBits(tile, (huff << extra) | difference,
                               huffBits + extra, reservoir, pending,
                               bytePos, word, good);
                } else {
                    appendBits(tile, huff, huffBits, reservoir, pending,
                               bytePos, word, good);
                    if (extra != 0u && good)
                        appendBits(tile, difference, extra, reservoir, pending,
                                   bytePos, word, good);
                }
            }
            previous = v;
            previousAbove = b;
        }
    }
    if (good && pending != 0u) {
        uint padded = ((reservoir << (8u - pending)) |
                       ((1u << (8u - pending)) - 1u)) & 255u;
        emitByte(tile, padded, bytePos, word, good);
        if (padded == 255u && good) emitByte(tile, 0u, bytePos, word, good);
    }
    uint bodyBytes = bytePos - bodyStart;
#ifdef JPEG_DIRECT_SLOT
    if (good) {
        emitByte(tile, 255u, bytePos, word, good);
        emitByte(tile, 217u, bytePos, word, good);
    }
#endif
    if (good && (bytePos & 3u) != 0u)
        bodies.words[tile * p.strideWords + (bytePos >> 2u)] = word;
    lengths.words[tile] = good ? bodyBytes : 0u;
}
)glsl";
constexpr char tiledPrefixShader[] = R"glsl(#version 450
layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer Lengths { uint words[]; } lengths;
layout(std430, binding = 1) writeonly buffer Offsets { uint words[]; } offsets;
layout(push_constant) uniform Constants { uint tiles; } p;
void main() {
    uint cursor = 0u;
    for (uint tile = 0u; tile < p.tiles; ++tile) {
        offsets.words[tile] = cursor;
        cursor += (lengths.words[tile] + 3u) >> 2u;
    }
    offsets.words[p.tiles] = cursor;
}
)glsl";
constexpr char tiledCompactShader[] = R"glsl(#version 450
layout(local_size_x = 8) in;
layout(std430, binding = 0) readonly buffer Bodies { uint words[]; } bodies;
layout(std430, binding = 1) readonly buffer Lengths { uint words[]; } lengths;
layout(std430, binding = 2) readonly buffer Offsets { uint words[]; } offsets;
layout(std430, binding = 3) writeonly buffer Compact { uint words[]; } compact;
layout(push_constant) uniform Constants {
    uint tiles; uint strideWords; uint capacityWords;
} p;
void main() {
    uint tile = gl_GlobalInvocationID.x;
    if (tile >= p.tiles) return;
    uint start = offsets.words[tile];
    uint count = (lengths.words[tile] + 3u) >> 2u;
    if (start + count > p.capacityWords) return;
    for (uint i = 0u; i < count; ++i)
        compact.words[start + i] = bodies.words[tile * p.strideWords + i];
}
)glsl";
constexpr char tiledJpegPrefixShader[] = R"glsl(#version 450
layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer Lengths { uint words[]; } lengths;
layout(std430, binding = 1) readonly buffer Table { uint values[]; } table;
layout(std430, binding = 2) writeonly buffer Offsets { uint words[]; } offsets;
layout(push_constant) uniform Constants { uint tiles; uint channels; } p;
void main() {
    uint cursor = 0u;
    uint headerBytes = 41u + table.values[1] + 5u * p.channels;
    for (uint tile = 0u; tile < p.tiles; ++tile) {
        offsets.words[tile] = cursor;
        cursor += (headerBytes + lengths.words[tile] + 5u) >> 2u;
    }
    offsets.words[p.tiles] = cursor;
}
)glsl";
constexpr char tiledJpegAssembleShader[] = R"glsl(#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer Bodies { uint words[]; } bodies;
layout(std430, binding = 1) readonly buffer Lengths { uint words[]; } lengths;
layout(std430, binding = 2) readonly buffer Offsets { uint words[]; } offsets;
layout(std430, binding = 3) readonly buffer Table { uint values[]; } table;
layout(std430, binding = 4) writeonly buffer Output { uint words[]; } outputData;
layout(push_constant) uniform Constants {
    uint tiles; uint strideWords; uint capacityWords;
    uint tileWidth; uint tileHeight; uint channels; uint slotWords;
    uint bits;
} p;
void emit(uint base, uint value, inout uint cursor, inout uint word) {
    word |= (value & 255u) << ((cursor & 3u) * 8u);
    ++cursor;
    if ((cursor & 3u) == 0u) {
        outputData.words[base + (cursor >> 2u) - 1u] = word;
        word = 0u;
    }
}
void emit16(uint base, uint value, inout uint cursor, inout uint word) {
    emit(base, value >> 8u, cursor, word);
    emit(base, value, cursor, word);
}
void main() {
    uint tile = gl_WorkGroupID.x;
    if (tile >= p.tiles) return;
    uint start = p.slotWords != 0u ? tile * p.slotWords : offsets.words[tile];
    uint end = p.slotWords != 0u ? start + p.slotWords :
                                   offsets.words[tile + 1u];
    if (end > p.capacityWords || lengths.words[tile] == 0u) return;
    uint headerBytes = 41u + table.values[1] + 5u * p.channels;
    uint bodyLength = lengths.words[tile];
    uint bodyBase = tile * p.strideWords;
    uint firstBodyWord = (headerBytes + 3u) >> 2u;
    uint lastBodyWord = (headerBytes + bodyLength) >> 2u;
    for (uint index = firstBodyWord + gl_LocalInvocationID.x;
         index < lastBodyWord; index += 64u) {
        uint byteOffset = (index << 2u) - headerBytes;
        uint source = bodyBase + (byteOffset >> 2u);
        uint shift = (byteOffset & 3u) << 3u;
        uint value = bodies.words[source] >> shift;
        if (shift != 0u) value |= bodies.words[source + 1u] << (32u - shift);
        outputData.words[start + index] = value;
    }
    if (gl_LocalInvocationID.x != 0u) return;
    uint cursor = 0u, word = 0u;
    emit(start, 255u, cursor, word); emit(start, 216u, cursor, word);
    emit(start, 255u, cursor, word); emit(start, 196u, cursor, word);
    emit16(start, 19u + table.values[1], cursor, word);
    emit(start, 0u, cursor, word);
    for (uint i = 0u; i < 16u; ++i)
        emit(start, table.values[2u + i], cursor, word);
    for (uint i = 0u; i < table.values[1]; ++i)
        emit(start, table.values[18u + i], cursor, word);
    emit(start, 255u, cursor, word); emit(start, 195u, cursor, word);
    emit16(start, 8u + 3u * p.channels, cursor, word);
    emit(start, p.bits, cursor, word);
    emit16(start, p.tileHeight, cursor, word);
    emit16(start, p.tileWidth, cursor, word);
    emit(start, p.channels, cursor, word);
    for (uint c = 0u; c < p.channels; ++c) {
        emit(start, c, cursor, word);
        emit(start, 17u, cursor, word);
        emit(start, 0u, cursor, word);
    }
    emit(start, 255u, cursor, word); emit(start, 218u, cursor, word);
    emit16(start, 6u + 2u * p.channels, cursor, word);
    emit(start, p.channels, cursor, word);
    for (uint c = 0u; c < p.channels; ++c) {
        emit(start, c, cursor, word);
        emit(start, 0u, cursor, word);
    }
    emit(start, table.values[0], cursor, word);
    emit(start, 0u, cursor, word); emit(start, 0u, cursor, word);
    for (uint i = 0u; cursor < (firstBodyWord << 2u); ++i) {
        uint value = bodies.words[bodyBase + (i >> 2u)] >> ((i & 3u) * 8u);
        emit(start, value, cursor, word);
    }
    cursor = lastBodyWord << 2u;
    for (uint i = cursor - headerBytes; i < bodyLength; ++i) {
        uint value = bodies.words[bodyBase + (i >> 2u)] >> ((i & 3u) * 8u);
        emit(start, value, cursor, word);
    }
    emit(start, 255u, cursor, word); emit(start, 217u, cursor, word);
    if ((cursor & 3u) != 0u)
        outputData.words[start + (cursor >> 2u)] = word;
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
  pl_pass tiledTexturePass = nullptr;
  pl_pass tiledDirectPass = nullptr;
  pl_pass tiledBufferPass = nullptr;
  pl_pass tiledHistogramBufferPass = nullptr;
  pl_pass tiledPrefixPass = nullptr, tiledCompactPass = nullptr;
  pl_pass tiledJpegPrefixPass = nullptr, tiledJpegAssemblePass = nullptr;
  pl_buf tiledBodies = nullptr, tiledLengths = nullptr;
  pl_buf tiledCompact = nullptr, tiledOffsets = nullptr;
  pl_buf tiledBodyReadback = nullptr, tiledLengthReadback = nullptr;
  pl_buf tiledTableReadback = nullptr;
  size_t lastTiledBodyBytes = 0;
  size_t lastTiledOutputBytes = 0;
  uint32_t lastTiledWidth = 0, lastTiledHeight = 0;
  uint32_t lastTiledChannels = 0, lastTiledBits = 0;
  uint32_t lastTiledTileWidth = 0;
  uint32_t lastTiledTileHeight = 0;
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
    pl_pass_destroy(gpu, &tiledTexturePass);
    pl_pass_destroy(gpu, &tiledDirectPass);
    pl_pass_destroy(gpu, &tiledBufferPass);
    pl_pass_destroy(gpu, &tiledHistogramBufferPass);
    pl_pass_destroy(gpu, &tiledPrefixPass);
    pl_pass_destroy(gpu, &tiledCompactPass);
    pl_pass_destroy(gpu, &tiledJpegPrefixPass);
    pl_pass_destroy(gpu, &tiledJpegAssemblePass);
    for (auto *x : {&tiledBodies, &tiledLengths,
                   &tiledBodyReadback, &tiledLengthReadback,
                   &tiledTableReadback, &tiledCompact, &tiledOffsets})
      pl_buf_destroy(gpu, x);
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
  bool initTiledTexture(pl_gpu sharedGpu) {
    if (tiledTexturePass)
      return gpu == sharedGpu && tiledDirectPass && tiledPrefixPass && tiledCompactPass &&
             tiledJpegPrefixPass && tiledJpegAssemblePass;
    if (gpu && gpu != sharedGpu) return false;
    gpu = sharedGpu;
    tiledTexturePass = makePass(
        gpu, tiledTextureShader,
        {{"srcTex", PL_DESC_SAMPLED_TEX, 0, PL_DESC_ACCESS_READONLY},
         {"bodies", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY},
         {"lengths", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY},
         {"table", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY}}, 9);
    std::string residentShader = tiledTextureShader;
    residentShader.insert(residentShader.find('\n') + 1u,
                          "#define JPEG_DIRECT_SLOT\n");
    tiledDirectPass = makePass(
        gpu, residentShader.c_str(),
        {{"srcTex", PL_DESC_SAMPLED_TEX, 0, PL_DESC_ACCESS_READONLY},
         {"bodies", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY},
         {"lengths", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY},
         {"table", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY}}, 9);
    tiledPrefixPass = makePass(
        gpu, tiledPrefixShader,
        {{"lengths", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
         {"offsets", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY}}, 1);
    tiledCompactPass = makePass(
        gpu, tiledCompactShader,
        {{"bodies", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
         {"lengths", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
         {"offsets", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_READONLY},
         {"compact", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_WRITEONLY}}, 3);
    tiledJpegPrefixPass = makePass(
        gpu, tiledJpegPrefixShader,
        {{"lengths", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
         {"table", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
         {"offsets", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY}}, 2);
    tiledJpegAssemblePass = makePass(
        gpu, tiledJpegAssembleShader,
        {{"bodies", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
         {"lengths", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READONLY},
         {"offsets", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_READONLY},
         {"table", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY},
         {"outputData", PL_DESC_BUF_STORAGE, 4, PL_DESC_ACCESS_WRITEONLY}}, 8);
    return tiledTexturePass && tiledDirectPass && tiledPrefixPass && tiledCompactPass &&
           tiledJpegPrefixPass && tiledJpegAssemblePass;
  }
  bool initTiledBuffer(pl_gpu sharedGpu) {
    if (!initTiledTexture(sharedGpu) || !init(sharedGpu)) return false;
    if (tiledBufferPass && tiledHistogramBufferPass) return true;
    if (!tiledBufferPass) {
      std::string shader = tiledTextureShader;
      shader.insert(shader.find('\n') + 1u, "#define JPEG_SRC_BUFFER\n");
      tiledBufferPass = makePass(
          gpu, shader.c_str(),
          {{"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
           {"bodies", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_WRITEONLY},
           {"lengths", PL_DESC_BUF_STORAGE, 2, PL_DESC_ACCESS_WRITEONLY},
           {"table", PL_DESC_BUF_STORAGE, 3, PL_DESC_ACCESS_READONLY}}, 9);
    }
    if (!tiledHistogramBufferPass) {
      std::string histogram = histogramShader;
      histogram.insert(histogram.find('\n') + 1u,
                       "#define JPEG_TILED_HISTOGRAM\n");
      tiledHistogramBufferPass = makePass(
          gpu, histogram.c_str(),
          {{"src", PL_DESC_BUF_STORAGE, 0, PL_DESC_ACCESS_READONLY},
           {"hist", PL_DESC_BUF_STORAGE, 1, PL_DESC_ACCESS_READWRITE}}, 6);
    }
    return tiledBufferPass && tiledHistogramBufferPass;
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
  // A resident consumer may synchronously feed the decoded image into the
  // same device's full-frame encoder before this tile encode returns.
  std::recursive_mutex mutex;
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
                 const JpegOutputConsumer *consume = nullptr,
                 pl_buf validation = nullptr) {
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
  const uint32_t sourcePrecision = packedInput ? bits : 16u;
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
  const size_t outputBufferBytes =
      (outputWords + kMetaWords + (validation ? 1u : 0u)) * 4u;
  const size_t sourceBytes = packedInput
      ? size_t(packedWordsPerRow) * height * 4u
      : (size_t(samples) + 1u) / 2u * 4u;
  if (sourceBytes > UINT32_MAX)
    return false;
  if (packedInput && (!packedWordsPerRow ||
      packedWordsPerRow < (uint64_t(width) * channels * sourcePrecision + 31u) / 32u ||
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
                            sourcePrecision,
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
                               bits, sourcePrecision,
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
  if (validation)
    pl_buf_copy(gpu, s.b[OutputStage], outputBufferBytes - 4u,
                validation, 0, 4u);
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
  if (validation &&
      *reinterpret_cast<const uint32_t *>(readback + outputBufferBytes - 4u) != 1u) {
    if (!mappedReadback) std::free(readback);
    return false;
  }
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

static bool encodeTiledVulkanTexture(
    const void *gpuHandle, const void *sourceHandle, uint32_t width,
    uint32_t height, uint32_t channels, uint32_t tileWidth,
    uint32_t tileHeight, std::vector<std::vector<uint8_t>>* tiles,
    VulkanJpegTiledPayload* contiguous,
    const JpegGpuTiledConsumer* residentConsumer = nullptr,
    bool packedBuffer = false, uint32_t sourceWordsPerRow = 0,
    uint32_t sourceBits = 16) {
  auto gpu = static_cast<pl_gpu>(const_cast<void *>(gpuHandle));
  auto texture = packedBuffer ? nullptr :
      static_cast<pl_tex>(const_cast<void *>(sourceHandle));
  auto inputBuffer = packedBuffer ?
      static_cast<pl_buf>(const_cast<void *>(sourceHandle)) : nullptr;
  if (!gpu || (!texture && !inputBuffer) ||
      (channels != 1u && channels != 3u) || !tileWidth || !tileHeight ||
      width % tileWidth || height % tileHeight ||
      tileWidth > 65535u || tileHeight > 65535u ||
      uint64_t(tileWidth) * tileHeight * channels > 65536u ||
      sourceBits < 8u || sourceBits > 16u ||
      (!packedBuffer && sourceBits != 16u))
    return false;
  if (packedBuffer) {
    const uint64_t minWords =
        (uint64_t(width) * channels * sourceBits + 31u) / 32u;
    if (!inputBuffer->params.storable || sourceWordsPerRow < minWords ||
        uint64_t(sourceWordsPerRow) * height * 4u > inputBuffer->params.size)
      return false;
  } else {
    if (!texture->params.sampleable || texture->params.w != int(width) ||
        texture->params.h != int(height)) return false;
    const auto format = texture->params.format;
    if (!format || format->opaque || format->type != PL_FMT_UNORM ||
        (channels == 1u ? format->num_components != 1
                        : format->num_components < 3) ||
        (channels == 1u ? format->texel_size != sizeof(uint16_t)
                        : format->texel_size < 3u * sizeof(uint16_t)))
      return false;
    for (uint32_t c = 0; c < channels; ++c)
      if (format->host_bits[c] != 16 || format->sample_order[c] != int(c))
        return false;
  }
  const uint64_t tileCount64 = uint64_t(width / tileWidth) *
                               (height / tileHeight);
  if (uint64_t(width) * height * channels > UINT32_MAX) return false;
  const uint64_t tileSamples = uint64_t(tileWidth) * tileHeight * channels;
  const uint64_t strideWords64 = (tileSamples * 3u + 3u) / 4u;
  const uint64_t bodyWords64 = tileCount64 * strideWords64;
  if (!tileCount64 || tileCount64 > UINT32_MAX ||
      strideWords64 > UINT32_MAX || bodyWords64 > UINT32_MAX ||
      bodyWords64 > SIZE_MAX / 4u ||
      (tileCount64 + 7u) / 8u > uint32_t(gpu->limits.max_dispatch[0]) ||
      bodyWords64 * 4u > gpu->limits.max_ssbo_size) {
    std::fprintf(stderr, "GPU tiled JPEG: capacity rejected tiles=%zu body=%zu max=%zu dispatch=%d\n",
                 size_t(tileCount64), size_t(bodyWords64 * 4u),
                 size_t(gpu->limits.max_ssbo_size), gpu->limits.max_dispatch[0]);
    return false;
  }
  auto &external = externalStates();
  std::lock_guard lock(external.mutex);
  auto &slot = external.states[gpu];
  if (!slot) slot = std::make_unique<State>();
  State &s = *slot;
  const bool profile = std::getenv("MOTIONCAM_GPU_TILED_JPEG_PROFILE") != nullptr;
  auto stageStarted = std::chrono::steady_clock::now();
  auto stage = [&](const char* name) {
    if (!profile) return;
    pl_gpu_finish(gpu);
    const auto now = std::chrono::steady_clock::now();
    std::fprintf(stderr, "GPU tiled JPEG stage channels=%u %s=%.3f ms\n",
                 channels, name,
                 std::chrono::duration<double, std::milli>(now - stageStarted).count());
    stageStarted = now;
  };
  if (!(packedBuffer ? s.initTiledBuffer(gpu) :
        s.initTiledTexture(gpu) && s.initTexture(gpu, channels == 3u))) {
    std::fprintf(stderr, "GPU tiled JPEG: shader initialization failed\n");
    return false;
  }
  const size_t bodyBytes = size_t(bodyWords64) * 4u;
  const size_t lengthBytes = size_t(tileCount64) * 4u;
  const bool compact = channels == 3u;
  const bool gpuAssemble = residentConsumer ||
      std::getenv("MOTIONCAM_GPU_TILED_JPEG_GPU_ASSEMBLE_DISABLE") == nullptr;
  const bool directResident = residentConsumer &&
      std::getenv("MOTIONCAM_GPU_TILED_JPEG_DIRECT_DISABLE") == nullptr;
  const bool sameLayout = s.lastTiledWidth == width &&
      s.lastTiledHeight == height && s.lastTiledChannels == channels &&
      s.lastTiledBits == sourceBits &&
      s.lastTiledTileWidth == tileWidth &&
      s.lastTiledTileHeight == tileHeight;
  const size_t compactBytes = (compact ? std::min(bodyBytes,
      sameLayout && s.lastTiledBodyBytes
          ? s.lastTiledBodyBytes + s.lastTiledBodyBytes / 4u + 1048576u
          : bodyBytes) : bodyBytes) & ~size_t{3};
  const size_t maxOutputBytes = bodyBytes + size_t(tileCount64) * 128u;
  const size_t outputBytes = (residentConsumer ? maxOutputBytes :
      gpuAssemble ? std::min(maxOutputBytes,
      sameLayout && s.lastTiledOutputBytes
          ? s.lastTiledOutputBytes + s.lastTiledOutputBytes / 4u + 1048576u
          : maxOutputBytes) : compactBytes) & ~size_t{3};
  if (outputBytes > gpu->limits.max_ssbo_size ||
      outputBytes / 4u > UINT32_MAX) return false;
  auto buffer = [&](pl_buf &target, size_t bytes, bool readback) {
    pl_buf_params params{};
    params.size = bytes;
    params.storable = !readback;
    params.host_readable = readback;
    params.host_mapped = readback;
    params.memory_type = readback ? PL_BUF_MEM_HOST : PL_BUF_MEM_DEVICE;
    return pl_buf_recreate(gpu, &target, &params);
  };
  if ((!directResident && !buffer(s.tiledBodies, bodyBytes, false)) ||
      !buffer(s.tiledLengths, lengthBytes, false) ||
      ((compact || gpuAssemble) &&
       !buffer(s.tiledCompact, outputBytes, false)) ||
      (!directResident && (compact || gpuAssemble) &&
       !buffer(s.tiledOffsets, lengthBytes + 4u, false)) ||
      (!residentConsumer &&
       (!buffer(s.tiledBodyReadback, outputBytes, true) ||
        !buffer(s.tiledLengthReadback, lengthBytes, true) ||
        !buffer(s.tiledTableReadback, 69u * 4u, true))) ||
      !s.buffer(Histogram, 8u * 18u * 4u) ||
      !s.buffer(HistogramZero, 8u * 18u * 4u, false, true) ||
      !s.buffer(Table, 69u * 4u)) {
    std::fprintf(stderr, "GPU tiled JPEG: buffer allocation failed body=%zu max=%zu\n",
                 bodyBytes, size_t(gpu->limits.max_ssbo_size));
    return false;
  }
  std::array<uint32_t, 8u * 18u> emptyHistogram{};
  pl_buf_write(gpu, s.b[HistogramZero], 0, emptyHistogram.data(),
               emptyHistogram.size() * 4u);
  pl_buf_copy(gpu, s.b[Histogram], 0, s.b[HistogramZero], 0,
              emptyHistogram.size() * 4u);
  uint32_t dimensions[6] = {width, height, channels, sourceBits, sourceBits,
                            packedBuffer ? sourceWordsPerRow : 0u};
  const uint32_t samples = width * height * channels;
  const uint32_t histInvocations = channels == 3u
      ? height * ((width + 15u) / 16u) * 2u : samples;
  if (packedBuffer) {
    pl_desc_binding histBindings[2]{};
    histBindings[0].object = inputBuffer;
    histBindings[1].object = s.b[Histogram];
    pl_pass_run_params histRun{};
    histRun.pass = s.tiledHistogramBufferPass;
    histRun.desc_bindings = histBindings;
    histRun.push_constants = dimensions;
    histRun.compute_groups[0] = int((samples + 255u) / 256u);
    histRun.compute_groups[1] = histRun.compute_groups[2] = 1;
    pl_pass_run(gpu, &histRun);
  } else
    s.runTexture(channels == 3u ? HistogramRgbTexturePass
                                : HistogramTexturePass,
                 texture, {Histogram}, dimensions,
                 int((histInvocations + 255u) / 256u));
  uint32_t sampleCount = samples;
  s.run(TablePass, {Histogram, Table}, &sampleCount, 1);
  stage("histogram_table");
  uint32_t constants[9] = {
      width, height, tileWidth, tileHeight, channels,
      uint32_t(strideWords64 + (directResident ? 32u : 0u)),
      uint32_t(tileCount64), sourceWordsPerRow, sourceBits};
  pl_desc_binding bindings[4]{};
  if (packedBuffer) bindings[0].object = inputBuffer;
  else bindings[0].object = texture;
  bindings[1].object = directResident ? s.tiledCompact : s.tiledBodies;
  bindings[2].object = s.tiledLengths;
  bindings[3].object = s.b[Table];
  pl_pass_run_params run{};
  run.pass = packedBuffer ? s.tiledBufferPass :
             directResident ? s.tiledDirectPass : s.tiledTexturePass;
  run.desc_bindings = bindings;
  run.push_constants = constants;
  run.compute_groups[0] = int((tileCount64 + 7u) / 8u);
  run.compute_groups[1] = run.compute_groups[2] = 1;
  pl_pass_run(gpu, &run);
  stage("entropy");
  if (directResident)
    return (*residentConsumer)(gpu, s.tiledCompact, s.tiledLengths,
                               s.b[Table], width, height, channels,
                               tileWidth, tileHeight, uint32_t(tileCount64),
                               uint32_t(strideWords64 + 32u));
  if (gpuAssemble) {
    if (!residentConsumer) {
      pl_desc_binding prefixBindings[3]{};
      prefixBindings[0].object = s.tiledLengths;
      prefixBindings[1].object = s.b[Table];
      prefixBindings[2].object = s.tiledOffsets;
      uint32_t prefixConstants[2] = {uint32_t(tileCount64), channels};
      pl_pass_run_params prefixRun{};
      prefixRun.pass = s.tiledJpegPrefixPass;
      prefixRun.desc_bindings = prefixBindings;
      prefixRun.push_constants = prefixConstants;
      prefixRun.compute_groups[0] = 1;
      prefixRun.compute_groups[1] = prefixRun.compute_groups[2] = 1;
      pl_pass_run(gpu, &prefixRun);
    }
    pl_desc_binding assembleBindings[5]{};
    assembleBindings[0].object = s.tiledBodies;
    assembleBindings[1].object = s.tiledLengths;
    assembleBindings[2].object = s.tiledOffsets;
    assembleBindings[3].object = s.b[Table];
    assembleBindings[4].object = s.tiledCompact;
    uint32_t assembleConstants[8] = {
        uint32_t(tileCount64), uint32_t(strideWords64),
        uint32_t(outputBytes / 4u), tileWidth, tileHeight, channels,
        residentConsumer ? uint32_t(strideWords64 + 32u) : 0u,
        sourceBits};
    pl_pass_run_params assembleRun{};
    assembleRun.pass = s.tiledJpegAssemblePass;
    assembleRun.desc_bindings = assembleBindings;
    assembleRun.push_constants = assembleConstants;
    assembleRun.compute_groups[0] = int(tileCount64);
    assembleRun.compute_groups[1] = assembleRun.compute_groups[2] = 1;
    pl_pass_run(gpu, &assembleRun);
    if (residentConsumer) {
      stage("gpu_jpeg_assemble");
      return (*residentConsumer)(gpu, s.tiledCompact,
                                 s.tiledLengths, s.b[Table], width, height,
                                 channels, tileWidth, tileHeight,
                                 uint32_t(tileCount64),
                                 uint32_t(strideWords64 + 32u));
    }
    pl_buf_copy(gpu, s.tiledBodyReadback, 0, s.tiledCompact, 0, outputBytes);
  } else if (compact) {
    pl_desc_binding prefixBindings[2]{};
    prefixBindings[0].object = s.tiledLengths;
    prefixBindings[1].object = s.tiledOffsets;
    uint32_t tileCount = uint32_t(tileCount64);
    pl_pass_run_params prefixRun{};
    prefixRun.pass = s.tiledPrefixPass;
    prefixRun.desc_bindings = prefixBindings;
    prefixRun.push_constants = &tileCount;
    prefixRun.compute_groups[0] = 1;
    prefixRun.compute_groups[1] = prefixRun.compute_groups[2] = 1;
    pl_pass_run(gpu, &prefixRun);
    pl_desc_binding compactBindings[4]{};
    compactBindings[0].object = s.tiledBodies;
    compactBindings[1].object = s.tiledLengths;
    compactBindings[2].object = s.tiledOffsets;
    compactBindings[3].object = s.tiledCompact;
    uint32_t compactConstants[3] = {
        tileCount, uint32_t(strideWords64), uint32_t(compactBytes / 4u)};
    pl_pass_run_params compactRun{};
    compactRun.pass = s.tiledCompactPass;
    compactRun.desc_bindings = compactBindings;
    compactRun.push_constants = compactConstants;
    compactRun.compute_groups[0] = int((tileCount64 + 7u) / 8u);
    compactRun.compute_groups[1] = compactRun.compute_groups[2] = 1;
    pl_pass_run(gpu, &compactRun);
    pl_buf_copy(gpu, s.tiledBodyReadback, 0, s.tiledCompact, 0,
                compactBytes);
  } else {
    pl_buf_copy(gpu, s.tiledBodyReadback, 0, s.tiledBodies, 0, bodyBytes);
  }
  pl_buf_copy(gpu, s.tiledLengthReadback, 0, s.tiledLengths, 0, lengthBytes);
  pl_buf_copy(gpu, s.tiledTableReadback, 0, s.b[Table], 0, 69u * 4u);
  stage("compact_copy");
  while (pl_buf_poll(gpu, s.tiledLengthReadback, UINT64_MAX)) {}
  while (pl_buf_poll(gpu, s.tiledBodyReadback, UINT64_MAX)) {}
  while (pl_buf_poll(gpu, s.tiledTableReadback, UINT64_MAX)) {}
  stage("readback_wait");
  if (!s.tiledBodyReadback->data || !s.tiledLengthReadback->data ||
      !s.tiledTableReadback->data) {
    std::fprintf(stderr, "GPU tiled JPEG: mapped readback unavailable\n");
    return false;
  }
  const auto *body = static_cast<const uint8_t *>(s.tiledBodyReadback->data);
  const auto *lengths = reinterpret_cast<const uint32_t *>(
      s.tiledLengthReadback->data);
  const auto *tableWords = reinterpret_cast<const uint32_t *>(
      s.tiledTableReadback->data);
  if (tableWords[1] == 0u || tableWords[1] > 17u ||
      tableWords[0] < 1u || tableWords[0] > 7u) return false;
  size_t compactUsed = 0;
  for (size_t tile = 0; tile < size_t(tileCount64); ++tile) {
    if (!lengths[tile] || lengths[tile] > strideWords64 * 4u ||
        compactUsed > bodyBytes -
            std::min(bodyBytes, size_t((lengths[tile] + 3u) & ~3u)))
      return false;
    compactUsed += (size_t(lengths[tile]) + 3u) & ~size_t{3};
  }
  if (compactUsed > bodyBytes || (!gpuAssemble && compact && compactUsed > compactBytes))
    return false;
  auto emit16 = [](std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 8u));
    out.push_back(uint8_t(value));
  };
  std::vector<uint8_t> header;
  header.reserve(96u);
  header.insert(header.end(), {0xff, 0xd8, 0xff, 0xc4});
  emit16(header, 19u + tableWords[1]);
  header.push_back(0u);
  for (uint32_t len = 0; len < 16; ++len)
    header.push_back(uint8_t(tableWords[2u + len]));
  for (uint32_t index = 0; index < tableWords[1]; ++index)
    header.push_back(uint8_t(tableWords[18u + index]));
  header.insert(header.end(), {0xff, 0xc3});
  emit16(header, 8u + 3u * channels);
  header.push_back(uint8_t(sourceBits));
  emit16(header, tileHeight);
  emit16(header, tileWidth);
  header.push_back(uint8_t(channels));
  for (uint32_t c = 0; c < channels; ++c) {
    header.push_back(uint8_t(c));
    header.push_back(0x11u);
    header.push_back(0u);
  }
  header.insert(header.end(), {0xff, 0xda});
  emit16(header, 6u + 2u * channels);
  header.push_back(uint8_t(channels));
  for (uint32_t c = 0; c < channels; ++c) {
    header.push_back(uint8_t(c));
    header.push_back(0u);
  }
  header.push_back(uint8_t(tableWords[0]));
  header.push_back(0u);
  header.push_back(0u);
  size_t totalJpegBytes = 0;
  for (size_t tile = 0; tile < size_t(tileCount64); ++tile) {
    if (totalJpegBytes > UINT32_MAX - header.size() -
            size_t(lengths[tile]) - 2u) return false;
    totalJpegBytes += header.size() + lengths[tile] + 2u;
  }
  VulkanJpegTiledPayload result;
  result.offsets.resize(size_t(tileCount64));
  result.lengths.resize(size_t(tileCount64));
  if (gpuAssemble) {
    size_t paddedBytes = 0;
    for (size_t tile = 0; tile < size_t(tileCount64); ++tile) {
      const size_t jpegBytes = header.size() + lengths[tile] + 2u;
      if (paddedBytes > outputBytes ||
          ((jpegBytes + 3u) & ~size_t{3}) > outputBytes - paddedBytes)
        return false;
      result.offsets[tile] = uint32_t(paddedBytes);
      result.lengths[tile] = uint32_t(jpegBytes);
      paddedBytes += (jpegBytes + 3u) & ~size_t{3};
    }
    result.bytes.assign(body, body + paddedBytes);
    s.lastTiledOutputBytes = paddedBytes;
  } else {
    result.bytes.resize(totalJpegBytes);
    size_t compactOffset = 0, jpegOffset = 0;
    for (size_t tile = 0; tile < size_t(tileCount64); ++tile) {
      if (!lengths[tile] || lengths[tile] > strideWords64 * 4u) {
        std::fprintf(stderr, "GPU tiled JPEG: tile %zu length=%u capacity=%zu\n",
                     tile, lengths[tile], size_t(strideWords64) * 4u);
        return false;
      }
      const size_t jpegBytes = header.size() + lengths[tile] + 2u;
      result.offsets[tile] = uint32_t(jpegOffset);
      result.lengths[tile] = uint32_t(jpegBytes);
      auto* jpeg = result.bytes.data() + jpegOffset;
      std::memcpy(jpeg, header.data(), header.size());
      const size_t bodyOffset = compact
          ? compactOffset : tile * size_t(strideWords64) * 4u;
      std::memcpy(jpeg + header.size(), body + bodyOffset, lengths[tile]);
      compactOffset += (size_t(lengths[tile]) + 3u) & ~size_t{3};
      jpeg[jpegBytes - 2u] = 0xff;
      jpeg[jpegBytes - 1u] = 0xd9;
      jpegOffset += jpegBytes;
    }
  }
  s.lastTiledBodyBytes = compactUsed;
  s.lastTiledWidth = width;
  s.lastTiledHeight = height;
  s.lastTiledChannels = channels;
  s.lastTiledBits = sourceBits;
  s.lastTiledTileWidth = tileWidth;
  s.lastTiledTileHeight = tileHeight;
  if (tiles) {
    tiles->resize(size_t(tileCount64));
    for (size_t tile = 0; tile < size_t(tileCount64); ++tile)
      (*tiles)[tile].assign(result.bytes.begin() + result.offsets[tile],
                            result.bytes.begin() + result.offsets[tile] +
                                result.lengths[tile]);
  }
  if (contiguous) *contiguous = std::move(result);
  stage("cpu_assemble");
  return true;
}

bool jpegEncodeTiledVulkanTexture(
    const void *gpu, const void *texture, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t tileWidth, uint32_t tileHeight,
    std::vector<std::vector<uint8_t>>& tiles) {
  return encodeTiledVulkanTexture(gpu, texture, width, height, channels,
                                 tileWidth, tileHeight, &tiles, nullptr);
}

bool jpegEncodeTiledVulkanTexture(
    const void *gpu, const void *texture, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t tileWidth, uint32_t tileHeight,
    VulkanJpegTiledPayload& payload) {
  return encodeTiledVulkanTexture(gpu, texture, width, height, channels,
                                 tileWidth, tileHeight, nullptr, &payload);
}

bool jpegEncodeTiledVulkanPackedBuffer(
    const void* gpu, const void* buffer, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t bits, uint32_t wordsPerRow,
    uint32_t tileWidth, uint32_t tileHeight,
    std::vector<std::vector<uint8_t>>& tiles) {
  return encodeTiledVulkanTexture(gpu, buffer, width, height, channels,
                                 tileWidth, tileHeight, &tiles, nullptr,
                                 nullptr, true, wordsPerRow, bits);
}

bool jpegEncodeTiledVulkanTextureResident(
    const void *gpu, const void *texture, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t tileWidth, uint32_t tileHeight,
    const JpegGpuTiledConsumer& consume) {
  return consume && encodeTiledVulkanTexture(
      gpu, texture, width, height, channels, tileWidth, tileHeight,
      nullptr, nullptr, &consume);
}

bool jpegEncodeFullyVulkanPackedBufferBorrowed(
    const void *gpuHandle, const void *bufferHandle, uint32_t width,
    uint32_t height, uint32_t channels, uint32_t bits,
    uint32_t wordsPerRow, const JpegOutputConsumer &consume,
    const void *validationBuffer) {
  auto gpu = static_cast<pl_gpu>(const_cast<void *>(gpuHandle));
  auto buffer = static_cast<pl_buf>(const_cast<void *>(bufferHandle));
  auto validation = static_cast<pl_buf>(const_cast<void *>(validationBuffer));
  if (!gpu || !buffer || !consume || !buffer->params.storable ||
      (validation && (!validation->params.storable || validation->params.size < 4u)) ||
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
                     width, height, channels, bits, &unused, &length, &consume,
                     validation);
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
bool jpegEncodeTiledVulkanTexture(
    const void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, std::vector<std::vector<uint8_t>>&) {
  return false;
}
bool jpegEncodeTiledVulkanTexture(
    const void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, VulkanJpegTiledPayload&) {
  return false;
}
bool jpegEncodeTiledVulkanPackedBuffer(
    const void*, const void*, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, std::vector<std::vector<uint8_t>>&) {
  return false;
}
bool jpegEncodeTiledVulkanTextureResident(
    const void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, const JpegGpuTiledConsumer&) {
  return false;
}
bool jpegEncodeFullyVulkanPackedBufferBorrowed(
    const void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, const JpegOutputConsumer &, const void *) {
  return false;
}
void releaseVulkanJpegFullEncodeDevice(const void *) {}
} // namespace motioncam
#endif
