// Limited-range, left-sited BT.709 NV12 and full-range nonlinear RGBA8.
typedef unsigned char byte;
__device__ byte sample8(float v) {return (byte)__float2int_rn(fminf(255.f,fmaxf(0.f,v)));}
__device__ float mix2(float a,float b,float p) {return a+(b-a)*p;}
extern "C" __global__ void unpack709(const byte* yp,int ys,const byte* uv,int us,
                                     byte* rgba,int rs,int w,int h) {
    int x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=w||y>=h)return;
    float cx=x*.5f,cy=fmaxf(0.f,y*.5f-.25f);
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
extern "C" __global__ void pack709(const byte* rgba,int rs,byte* yp,int ys,byte* uv,int us,int w,int h) {
    int x=2*(blockIdx.x*blockDim.x+threadIdx.x),y=2*(blockIdx.y*blockDim.y+threadIdx.y);
    if(x>=w||y>=h)return;
    float cb=0,cr=0;
    for(int j=0;j<2;++j)for(int i=0;i<2;++i){
        const byte* p=rgba+(y+j)*rs+4*(x+i);
        float l=.2126f*p[0]+.7152f*p[1]+.0722f*p[2];
        yp[(y+j)*ys+x+i]=sample8(16.f+l*(219.f/255.f));
    }
    // A horizontal low-pass centered on the left luma sample, averaged vertically.
    for(int j=0;j<2;++j)for(int i=-1;i<=1;++i){
        const byte* p=rgba+(y+j)*rs+4*max(0,x+i);
        float l=.2126f*p[0]+.7152f*p[1]+.0722f*p[2];
        float weight=(i==0?.25f:.125f);
        cb+=weight*(p[2]-l)/1.8556f;cr+=weight*(p[0]-l)/1.5748f;
    }
    uv[(y/2)*us+x]=sample8(128.f+cb*(224.f/255.f));
    uv[(y/2)*us+x+1]=sample8(128.f+cr*(224.f/255.f));
}
