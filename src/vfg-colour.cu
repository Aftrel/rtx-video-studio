// Limited-range, left-sited BT.709 NV12 and full-range nonlinear RGBA8.
typedef unsigned char byte;
__device__ byte sample8(float v) {return (byte)__float2int_rn(fminf(255.f,fmaxf(0.f,v)));}
__device__ float mix2(float a,float b,float p) {return a+(b-a)*p;}
extern "C" __global__ void unpack709(const byte* yp,int ys,const byte* uv,int us,
                                     byte* rgba,int rs,int w,int h,int flags) {
    int x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=w||y>=h)return;
    float cx=x*.5f,cy=fmaxf(0.f,y*.5f-((flags&1)?0.f:.25f));
    int x0=(int)cx,y0=(int)cy,x1=min(x0+1,w/2-1),y1=min(y0+1,h/2-1);
    float c[2];
    for(int k=0;k<2;k++){
        float top=mix2(uv[y0*us+2*x0+k],uv[y0*us+2*x1+k],cx-x0);
        float bottom=mix2(uv[y1*us+2*x0+k],uv[y1*us+2*x1+k],cx-x0);
        c[k]=(mix2(top,bottom,cy-y0)-128.f)*(255.f/224.f);
    }
    float l=(yp[y*ys+x]-16.f)*(255.f/219.f);
    byte* dst=rgba+y*rs+4*x;
    dst[0]=sample8(l+1.5748f*c[1]);dst[1]=sample8(l-.187324f*c[0]-.468124f*c[1]);
    dst[2]=sample8(l+1.8556f*c[0]);dst[3]=255;
}
extern "C" __global__ void pack709(const byte* rgba,int rs,byte* yp,int ys,byte* uv,int us,int w,int h,int flags) {
    int x=2*(blockIdx.x*blockDim.x+threadIdx.x),y=2*(blockIdx.y*blockDim.y+threadIdx.y);
    if(x>=w||y>=h)return;
    float cb=0,cr=0;
    for(int j=0;j<2;++j)for(int i=0;i<2;++i){
        const byte* p=rgba+(y+j)*rs+4*(x+i);
        float l=.2126f*p[0]+.7152f*p[1]+.0722f*p[2];
        yp[(y+j)*ys+x+i]=sample8(16.f+l*(219.f/255.f));
    }
    // A horizontal low-pass centered on the left luma sample, averaged vertically.
    for(int j=(flags&1)?-1:0;j<=1;++j)for(int i=-1;i<=1;++i){
        const byte* p=rgba+max(0,y+j)*rs+4*max(0,x+i);
        float l=.2126f*p[0]+.7152f*p[1]+.0722f*p[2];
        float weight=(i==0?.5f:.25f)*((flags&1)?(j==0?.5f:.25f):.5f);
        cb+=weight*(p[2]-l)/1.8556f;cr+=weight*(p[0]-l)/1.5748f;
    }
    uv[(y/2)*us+x]=sample8(128.f+cb*(224.f/255.f));
    uv[(y/2)*us+x+1]=sample8(128.f+cr*(224.f/255.f));
}

// Normalize noncanonical SDR on-device before feeding the existing BT.709 pipeline.
// Chroma stays on its original grid. Luma needs the chroma at each luma position.
__device__ float readSDR(const byte* plane,int stride,int x,int y,int ten){
    return ten?((const unsigned short*)(plane+y*stride))[x]/256.f:plane[y*stride+x];
}
__device__ void writeSDR(byte* plane,int stride,int x,int y,float level,int ten){
    if(ten)((unsigned short*)(plane+y*stride))[x]=__float2uint_rn(fminf(1023.f,fmaxf(0.f,level*4)))<<6;
    else plane[y*stride+x]=sample8(level);
}
__device__ float interpUV(const byte* uv,int stride,int x,int y,int w,int h,int k,int ten,int top) {
    float fx=x*.5f,fy=fmaxf(0.f,y*.5f-(top?0.f:.25f));
    int i=(int)fx,j=(int)fy,ii=min(i+1,w/2-1),jj=min(j+1,h/2-1);
    return mix2(mix2(readSDR(uv,stride,2*i+k,j,ten),readSDR(uv,stride,2*ii+k,j,ten),fx-i),
                mix2(readSDR(uv,stride,2*i+k,jj,ten),readSDR(uv,stride,2*ii+k,jj,ten),fx-i),fy-j);
}
extern "C" __global__ void normalizeSDR(const byte* y,int ys,const byte* uv,int us,byte* yo,int os,byte* uvo,int cs,int w,int h,int full,int sd,int ten,int top) {
    int x=blockIdx.x*blockDim.x+threadIdx.x,j=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=w||j>=h)return;
    float kr=sd?.299f:.2126f,kb=sd?.114f:.0722f,kg=1.f-kr-kb;
    float peak=ten?255.75f:255.f,chroma=full?peak:224.f;
    float red=2*(1-kr),blue=2*(1-kb);
    float deltaU=(.0722f-.7152f*kb/kg)*blue,deltaV=(.2126f-.7152f*kr/kg)*red;
    float u=(interpUV(uv,us,x,j,w,h,0,ten,top)-128)/chroma,v=(interpUV(uv,us,x,j,w,h,1,ten,top)-128)/chroma;
    float l=(readSDR(y,ys,x,j,ten)-(full?0:16))/(full?peak:219);
    writeSDR(yo,os,x,j,16+219*(l+deltaU*u+deltaV*v),ten);
    if(!(j%2) && !(x%2)){
        u=(readSDR(uv,us,x,j/2,ten)-128)/chroma;v=(readSDR(uv,us,x+1,j/2,ten)-128)/chroma;
        float d=deltaU*u+deltaV*v;
        writeSDR(uvo,cs,x,j/2,128+224*(blue*u-d)/1.8556f,ten);
        writeSDR(uvo,cs,x+1,j/2,128+224*(red*v-d)/1.5748f,ten);
    }
}

// P010 limited-range BT.2020 non-constant luminance, retaining the PQ/HLG
// transfer throughout. LSFG uses FP16 textures; Maxine uses packed RGB10A2.
__device__ unsigned short floatHalf(float x){unsigned short r;asm("cvt.rn.f16.f32 %0, %1;":"=h"(r):"f"(x));return r;}
__device__ float halfFloat(unsigned short x){float r;asm("cvt.f32.f16 %0, %1;":"=f"(r):"h"(x));return r;}
__device__ unsigned int code10(float x){return __float2uint_rn(fminf(1023.f,fmaxf(0.f,x)));}
__device__ float hdrSample(const byte* plane,int stride,int x,int y){return ((const unsigned short*)(plane+y*stride))[x]>>6;}
template<bool packed> __device__ void unpackHdr(const byte* yp,int ys,const byte* uv,int us,byte* rgb,int rs,int w,int h,int flags){
    bool top=flags&1,sdr=flags&2;
    float kr=sdr?.2126f:.2627f,kb=sdr?.0722f:.0593f;
    int x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;if(x>=w||y>=h)return;
    float fx=x*.5f,fy=fmaxf(0.f,y*.5f-(top?0.f:.25f));int i=(int)fx,j=(int)fy;
    float c[2];for(int k=0;k<2;k++){
        int a=2*i+k,b=2*min(i+1,w/2-1)+k,jj=min(j+1,h/2-1);
        c[k]=(mix2(mix2(hdrSample(uv,us,a,j),hdrSample(uv,us,b,j),fx-i),
                    mix2(hdrSample(uv,us,a,jj),hdrSample(uv,us,b,jj),fx-i),fy-j)-512)/896;
    }
    float l=(hdrSample(yp,ys,x,y)-64)/876,r=l+2*(1-kr)*c[1],b=l+2*(1-kb)*c[0],g=(l-kr*r-kb*b)/(1-kr-kb);
    if(packed)((unsigned*)(rgb+y*rs))[x]=code10(r*1023)|(code10(g*1023)<<10)|(code10(b*1023)<<20)|0xc0000000u;
    else {auto p=(unsigned short*)(rgb+y*rs)+4*x;p[0]=floatHalf(r);p[1]=floatHalf(g);p[2]=floatHalf(b);p[3]=floatHalf(1);}
}
template<bool packed> __device__ float3 readHdr(const byte* rgb,int rs,int x,int y){
    if(packed){unsigned p=((const unsigned*)(rgb+y*rs))[x];return make_float3((p&1023)/1023.f,((p>>10)&1023)/1023.f,((p>>20)&1023)/1023.f);}
    auto p=(const unsigned short*)(rgb+y*rs)+4*x;return make_float3(halfFloat(p[0]),halfFloat(p[1]),halfFloat(p[2]));
}
__device__ float hdrLuma(float3 p,float kr,float kb){return kr*p.x+(1-kr-kb)*p.y+kb*p.z;}
template<bool packed> __device__ void packHdr(const byte* rgb,int rs,byte* yp,int ys,byte* uv,int us,int w,int h,int flags){
    bool top=flags&1,sdr=flags&2;
    float kr=sdr?.2126f:.2627f,kb=sdr?.0722f:.0593f;
    int x=2*(blockIdx.x*blockDim.x+threadIdx.x),y=2*(blockIdx.y*blockDim.y+threadIdx.y);if(x>=w||y>=h)return;
    for(int j=0;j<2;j++) for(int i=0;i<2;i++)
        ((unsigned short*)(yp+(y+j)*ys))[x+i]=code10(64+876*hdrLuma(readHdr<packed>(rgb,rs,x+i,y+j),kr,kb))<<6;
    float u=0,v=0;
    for(int j=(top?-1:0);j<=1;j++)for(int i=-1;i<=1;i++){
        float3 p=readHdr<packed>(rgb,rs,max(0,x+i),max(0,y+j));float l=hdrLuma(p,kr,kb);
        float wt=(i==0?.5f:.25f)*(top?(j==0?.5f:.25f):.5f);
        u+=wt*(p.z-l)/(2*(1-kb));v+=wt*(p.x-l)/(2*(1-kr));
    }
    auto dst=(unsigned short*)(uv+(y/2)*us);dst[x]=code10(512+896*u)<<6;dst[x+1]=code10(512+896*v)<<6;
}
#define HDR_KERNELS(suffix,packed) \
extern "C" __global__ void unpack##suffix(const byte* y,int ys,const byte* uv,int us,byte* rgb,int rs,int w,int h,int top){unpackHdr<packed>(y,ys,uv,us,rgb,rs,w,h,top);} \
extern "C" __global__ void pack##suffix(const byte* rgb,int rs,byte* y,int ys,byte* uv,int us,int w,int h,int top){packHdr<packed>(rgb,rs,y,ys,uv,us,w,h,top);}
HDR_KERNELS(Half,false)
HDR_KERNELS(RGB10,true)
