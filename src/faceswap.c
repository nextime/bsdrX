/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3 or (at your option) any
 * later version. See <https://www.gnu.org/licenses/>.
 */
/* faceswap.c — in-process face swap via insightface-compatible ONNX (SCRFD + ArcFace + inswapper).
 * See faceswap.h. Compiled only with BSDR_HAVE_ONNX; a stub otherwise. */
#include "bsdr/faceswap.h"
#include "bsdr/log.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#ifdef BSDR_HAVE_ONNX
#include "onnxruntime_c_api.h"
#ifdef _WIN32
#  include <windows.h>
#endif
#ifdef __ANDROID__
#  include <dlfcn.h>
#endif

/* arcface 5-point destination template (for a 112x112 aligned face). */
static const float ARCFACE_DST[5][2] = {
    { 38.2946f, 51.6963f }, { 73.5318f, 51.5014f }, { 56.0252f, 71.7366f },
    { 41.5493f, 92.3655f }, { 70.7299f, 92.2041f }
};

#define DET_SIZE 640
#define MAX_FACES 8
#define DET_THRESH 0.5f
#define NMS_THRESH 0.4f

typedef struct { float x1,y1,x2,y2,score; float kps[5][2]; } face_t;

struct bsdr_faceswap {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *opts;
    OrtSession *det, *rec, *swp;      /* SCRFD, ArcFace, inswapper */
    OrtMemoryInfo *mem;
    char *det_in, *rec_in, *swp_in_t, *swp_in_s;
    char *det_out[9], *rec_out, *swp_out;
    float emap[512*512];              /* inswapper "buff2fs" projection */
    int   have_emap;
    float latent[512];                /* source identity, post-emap+normalize */
    int   have_source;
    char  status[128];
    const char *ep;
};

/* ---- ORT status helpers ---- */
static int ob(const OrtApi *o, OrtStatus *s, const char *w) {
    if (!s) return 0;
    BSDR_WARN("bsdr.faceswap", "%s: %s", w, o->GetErrorMessage(s));
    o->ReleaseStatus(s);
    return 1;
}
static void od(const OrtApi *o, OrtStatus *s) { if (s) o->ReleaseStatus(s); }

/* ---- small image ops (packed RGB, 3 bytes/px) ---- */
static inline void rgb_at(const uint8_t *img, int w, int h, int x, int y, float out[3]) {
    if (x < 0) x = 0; else if (x >= w) x = w-1;
    if (y < 0) y = 0; else if (y >= h) y = h-1;
    const uint8_t *p = img + ((size_t)y*w + x)*3;
    out[0]=p[0]; out[1]=p[1]; out[2]=p[2];
}
static inline void rgb_bilinear(const uint8_t *img, int w, int h, float fx, float fy, float out[3]) {
    int x0=(int)floorf(fx), y0=(int)floorf(fy); float ax=fx-x0, ay=fy-y0;
    float a[3],b[3],c[3],d[3];
    rgb_at(img,w,h,x0,y0,a); rgb_at(img,w,h,x0+1,y0,b); rgb_at(img,w,h,x0,y0+1,c); rgb_at(img,w,h,x0+1,y0+1,d);
    for (int k=0;k<3;k++) out[k]=(a[k]*(1-ax)+b[k]*ax)*(1-ay)+(c[k]*(1-ax)+d[k]*ax)*ay;
}

/* ---- 2x2 SVD (analytic) for the Umeyama similarity estimate ---- */
static void svd2(const float A[4], float U[4], float S[2], float V[4]) {
    float a=A[0],b=A[1],c=A[2],d=A[3];
    float e=(a+d)*0.5f, f=(a-d)*0.5f, g=(c+b)*0.5f, h=(c-b)*0.5f;
    float q=sqrtf(e*e+h*h), r=sqrtf(f*f+g*g);
    float sx=q+r, sy=q-r;
    float a1=atan2f(g,f), a2=atan2f(h,e);
    float theta=(a2-a1)*0.5f, phi=(a2+a1)*0.5f;
    float ct=cosf(theta), st=sinf(theta), cp=cosf(phi), sp=sinf(phi);
    U[0]=cp; U[1]=-sp; U[2]=sp; U[3]=cp;
    S[0]=fabsf(sx); S[1]=fabsf(sy);
    /* V^T rows; fold signs into V so U*S*V^T = A */
    float s1 = sx<0?-1.f:1.f, s2 = sy<0?-1.f:1.f;
    V[0]=s1*ct; V[1]=s1*(-st); V[2]=s2*st; V[3]=s2*ct;   /* V stored row-major (V^T applied later) */
}

/* Estimate a similarity transform M (2x3) mapping src(5x2) -> dst(5x2). Umeyama. */
static void umeyama(const float src[5][2], const float dst[5][2], float M[6]) {
    float ms[2]={0,0}, md[2]={0,0};
    for (int i=0;i<5;i++){ ms[0]+=src[i][0]; ms[1]+=src[i][1]; md[0]+=dst[i][0]; md[1]+=dst[i][1]; }
    ms[0]/=5; ms[1]/=5; md[0]/=5; md[1]/=5;
    float cov[4]={0,0,0,0}, var=0;
    for (int i=0;i<5;i++){
        float sx=src[i][0]-ms[0], sy=src[i][1]-ms[1];
        float dx=dst[i][0]-md[0], dy=dst[i][1]-md[1];
        cov[0]+=dx*sx; cov[1]+=dx*sy; cov[2]+=dy*sx; cov[3]+=dy*sy;
        var+=sx*sx+sy*sy;
    }
    for (int i=0;i<4;i++) cov[i]/=5;
    var/=5;
    float U[4],S[2],V[4]; svd2(cov,U,S,V);
    float det = cov[0]*cov[3]-cov[1]*cov[2];
    float d1=1.f, d2 = det<0?-1.f:1.f;
    /* R = U * diag(d) * V  (V already row-major = V^T of the decomposition) */
    float R[4];
    R[0]=U[0]*d1*V[0]+U[1]*d2*V[2];
    R[1]=U[0]*d1*V[1]+U[1]*d2*V[3];
    R[2]=U[2]*d1*V[0]+U[3]*d2*V[2];
    R[3]=U[2]*d1*V[1]+U[3]*d2*V[3];
    float scale = var>1e-12f ? (S[0]*d1+S[1]*d2)/var : 1.f;
    M[0]=scale*R[0]; M[1]=scale*R[1]; M[2]=md[0]-scale*(R[0]*ms[0]+R[1]*ms[1]);
    M[3]=scale*R[2]; M[4]=scale*R[3]; M[5]=md[1]-scale*(R[2]*ms[0]+R[3]*ms[1]);
}

/* invert a 2x3 affine */
static int inv_affine(const float M[6], float Mi[6]) {
    float det = M[0]*M[4]-M[1]*M[3];
    if (fabsf(det)<1e-12f) return -1;
    float id=1.f/det;
    Mi[0]= M[4]*id; Mi[1]=-M[1]*id; Mi[3]=-M[3]*id; Mi[4]= M[0]*id;
    Mi[2]=-(Mi[0]*M[2]+Mi[1]*M[5]); Mi[5]=-(Mi[3]*M[2]+Mi[4]*M[5]);
    return 0;
}

/* Warp `src` (packed RGB, sw x sh) into an out x out aligned crop using forward map M (out<-? ):
 * we sample src at M_inv(dst). Returns aligned RGB. */
static void warp_crop(const uint8_t *src, int sw, int sh, const float M[6], int out, uint8_t *dst) {
    float Mi[6]; if (inv_affine(M, Mi)!=0){ memset(dst,0,(size_t)out*out*3); return; }
    for (int y=0;y<out;y++) for (int x=0;x<out;x++){
        float sx=Mi[0]*x+Mi[1]*y+Mi[2], sy=Mi[3]*x+Mi[4]*y+Mi[5];
        float px[3]; rgb_bilinear(src,sw,sh,sx,sy,px);
        uint8_t *o=dst+((size_t)y*out+x)*3; o[0]=(uint8_t)(px[0]+.5f); o[1]=(uint8_t)(px[1]+.5f); o[2]=(uint8_t)(px[2]+.5f);
    }
}

/* ---- ORT run helpers ---- */
static OrtValue *mk_input(bsdr_faceswap *fs, const float *data, const int64_t *dims, int ndim) {
    OrtValue *v=NULL; size_t n=1; for(int i=0;i<ndim;i++) n*=(size_t)dims[i];
    if (ob(fs->ort, fs->ort->CreateTensorWithDataAsOrtValue(fs->mem,(void*)data,n*sizeof(float),
             dims,ndim,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&v),"CreateTensor")) return NULL;
    return v;
}

/* NCHW blob from a packed-RGB square, with (val*scale - mean)*... i.e. (val-mean)/std, optional BGR. */
static void to_blob(const uint8_t *rgb, int s, float mean, float std, float *out) {
    int hw=s*s;
    for (int y=0;y<s;y++) for (int x=0;x<s;x++){
        const uint8_t *p=rgb+((size_t)y*s+x)*3;
        out[0*hw + y*s+x]=((float)p[0]-mean)/std;
        out[1*hw + y*s+x]=((float)p[1]-mean)/std;
        out[2*hw + y*s+x]=((float)p[2]-mean)/std;
    }
}

/* ---- SCRFD detection ---- */
static float iou(const face_t *a, const face_t *b){
    float xx1=fmaxf(a->x1,b->x1),yy1=fmaxf(a->y1,b->y1),xx2=fminf(a->x2,b->x2),yy2=fminf(a->y2,b->y2);
    float w=fmaxf(0,xx2-xx1),h=fmaxf(0,yy2-yy1),inter=w*h;
    float ua=(a->x2-a->x1)*(a->y2-a->y1)+(b->x2-b->x1)*(b->y2-b->y1)-inter;
    return ua>0?inter/ua:0;
}

static int detect(bsdr_faceswap *fs, const uint8_t *rgb, int w, int h, face_t *faces, int maxf) {
    const OrtApi *o=fs->ort;
    /* letterbox to 640x640 (scale keeps aspect, pad bottom/right) */
    float scale = fminf((float)DET_SIZE/w,(float)DET_SIZE/h);
    int nw=(int)(w*scale), nh=(int)(h*scale);
    uint8_t *lb=calloc((size_t)DET_SIZE*DET_SIZE*3,1);
    if (!lb) return 0;
    for (int y=0;y<nh;y++) for (int x=0;x<nw;x++){
        float px[3]; rgb_bilinear(rgb,w,h,x/scale,y/scale,px);
        uint8_t *d=lb+((size_t)y*DET_SIZE+x)*3; d[0]=(uint8_t)px[0]; d[1]=(uint8_t)px[1]; d[2]=(uint8_t)px[2];
    }
    float *blob=malloc((size_t)3*DET_SIZE*DET_SIZE*sizeof(float));
    to_blob(lb,DET_SIZE,127.5f,128.0f,blob);   /* SCRFD: RGB, (x-127.5)/128, NCHW */
    free(lb);
    int64_t dims[4]={1,3,DET_SIZE,DET_SIZE};
    OrtValue *in=mk_input(fs,blob,dims,4);
    OrtValue *outs[9]={0};
    const char *innames[1]={fs->det_in};
    int nf=0;
    if (in && !ob(o,o->Run(fs->det,NULL,innames,(const OrtValue*const*)&in,1,
                           (const char*const*)fs->det_out,9,outs),"det Run")) {
        const int strides[3]={8,16,32};
        for (int si=0; si<3 && nf<maxf; si++) {
            int stride=strides[si], gw=DET_SIZE/stride, gh=DET_SIZE/stride, na=2;
            float *score,*bbox,*kps;
            od(o,o->GetTensorMutableData(outs[si],(void**)&score));
            od(o,o->GetTensorMutableData(outs[3+si],(void**)&bbox));
            od(o,o->GetTensorMutableData(outs[6+si],(void**)&kps));
            int idx=0;
            for (int gy=0;gy<gh;gy++) for (int gx=0;gx<gw;gx++) for (int a=0;a<na;a++,idx++){
                if (score[idx] < DET_THRESH) continue;
                float cx=(float)gx*stride, cy=(float)gy*stride;
                float *bp=bbox+idx*4;
                face_t f; f.score=score[idx];
                f.x1=(cx-bp[0]*stride)/scale; f.y1=(cy-bp[1]*stride)/scale;
                f.x2=(cx+bp[2]*stride)/scale; f.y2=(cy+bp[3]*stride)/scale;
                float *kp=kps+idx*10;
                for (int k=0;k<5;k++){ f.kps[k][0]=(cx+kp[2*k]*stride)/scale; f.kps[k][1]=(cy+kp[2*k+1]*stride)/scale; }
                if (nf<maxf) faces[nf++]=f;
            }
        }
    }
    if (getenv("BSDR_FS_DEBUG")) {
        for (int i=0;i<9;i++){ if(!outs[i])continue; float *dp; od(o,o->GetTensorMutableData(outs[i],(void**)&dp));
            OrtTensorTypeAndShapeInfo *ti=NULL; od(o,o->GetTensorTypeAndShape(outs[i],&ti)); size_t cnt=0; od(o,o->GetTensorShapeElementCount(ti,&cnt)); o->ReleaseTensorTypeAndShapeInfo(ti);
            float mx=-1e9; for(size_t z=0;z<cnt;z++) if(dp[z]>mx)mx=dp[z];
            BSDR_INFO("bsdr.faceswap","  out[%d] '%s' count=%zu max=%.3f",i,fs->det_out[i],cnt,mx); }
        float mx=0; for (int i=0;i<nf;i++) if (faces[i].score>mx) mx=faces[i].score;
        BSDR_INFO("bsdr.faceswap","detect: %d raw hits, max score %.3f (thr %.2f)", nf, mx, DET_THRESH);
    }
    for (int i=0;i<9;i++) if (outs[i]) o->ReleaseValue(outs[i]);
    if (in) o->ReleaseValue(in);
    free(blob);
    /* NMS (greedy by score) */
    for (int i=0;i<nf;i++) for (int j=i+1;j<nf;j++) if (faces[j].score>faces[i].score){ face_t t=faces[i]; faces[i]=faces[j]; faces[j]=t; }
    int keep=0; int used[MAX_FACES]={0};
    face_t out2[MAX_FACES];
    for (int i=0;i<nf;i++){ if (used[i]) continue; out2[keep++]=faces[i];
        for (int j=i+1;j<nf;j++) if (!used[j] && iou(&faces[i],&faces[j])>NMS_THRESH) used[j]=1; }
    memcpy(faces,out2,sizeof(face_t)*keep);
    return keep;
}

/* Align a face to `size` (112 or 128) using its 5 kps; fills M (frame<-aligned inverse handled by warp). */
static void align_face(const uint8_t *rgb, int w, int h, const face_t *f, int size, uint8_t *aligned, float M[6]) {
    float dst[5][2]; float ratio = (size%112==0)? size/112.f : size/128.f;
    float diff = (size%112==0)? 0.f : 8.f*ratio;
    for (int i=0;i<5;i++){ dst[i][0]=ARCFACE_DST[i][0]*ratio+diff; dst[i][1]=ARCFACE_DST[i][1]*ratio; }
    float src[5][2]; for (int i=0;i<5;i++){ src[i][0]=f->kps[i][0]; src[i][1]=f->kps[i][1]; }
    umeyama(src,dst,M);                 /* M maps src(frame) -> dst(aligned) */
    warp_crop(rgb,w,h,M,size,aligned);
}

/* ArcFace embedding of an aligned 112 face -> normalized 512. */
static int embed(bsdr_faceswap *fs, const uint8_t *aligned112, float out512[512]) {
    const OrtApi *o=fs->ort;
    float *blob=malloc(3*112*112*sizeof(float));
    to_blob(aligned112,112,127.5f,127.5f,blob);
    int64_t dims[4]={1,3,112,112}; OrtValue *in=mk_input(fs,blob,dims,4); OrtValue *ov=NULL;
    int rc=-1;
    const char *innames[1]={fs->rec_in}, *outnames[1]={fs->rec_out};
    if (in && !ob(o,o->Run(fs->rec,NULL,innames,(const OrtValue*const*)&in,1,outnames,1,&ov),"rec Run")) {
        float *emb; od(o,o->GetTensorMutableData(ov,(void**)&emb));
        float n=0; for(int i=0;i<512;i++) n+=emb[i]*emb[i]; n=sqrtf(n)+1e-9f;
        for(int i=0;i<512;i++) out512[i]=emb[i]/n;
        rc=0;
    }
    if (ov) o->ReleaseValue(ov);
    if (in) o->ReleaseValue(in);
    free(blob);
    return rc;
}

/* soft-blend the swapped 128 face back into the frame via the inverse of M. */
static void paste_back(uint8_t *rgb, int w, int h, const float *fake /*3x128x128, RGB [0,1]*/, const float M[6]) {
    /* iterate the frame region covered by the aligned box; for each pixel map frame->aligned via M and
     * sample the fake if inside, with a feathered mask that fades near the 128 border. */
    const int S=128, hw=S*S;
    /* bounding box of the aligned square back-projected into the frame */
    float Mi[6]; if (inv_affine(M,Mi)!=0) return;
    float corners[4][2]={{0,0},{S,0},{0,S},{S,S}}; float minx=1e9,miny=1e9,maxx=-1e9,maxy=-1e9;
    for(int i=0;i<4;i++){ float fx=Mi[0]*corners[i][0]+Mi[1]*corners[i][1]+Mi[2];
                          float fy=Mi[3]*corners[i][0]+Mi[4]*corners[i][1]+Mi[5];
                          minx=fminf(minx,fx);miny=fminf(miny,fy);maxx=fmaxf(maxx,fx);maxy=fmaxf(maxy,fy); }
    int x0=(int)fmaxf(0,minx), y0=(int)fmaxf(0,miny), x1=(int)fminf(w-1,maxx), y1=(int)fminf(h-1,maxy);
    const float feather=12.f;
    for (int y=y0;y<=y1;y++) for (int x=x0;x<=x1;x++){
        float ax=M[0]*x+M[1]*y+M[2], ay=M[3]*x+M[4]*y+M[5];
        if (ax<0||ax>=S||ay<0||ay>=S) continue;
        float edge=fminf(fminf(ax,S-ax),fminf(ay,S-ay));
        float m=edge>=feather?1.f:(edge<0?0.f:edge/feather);
        if (m<=0) continue;
        int ix=(int)ax, iy=(int)ay;      /* nearest-sample the fake (already smooth) */
        float fr=fake[0*hw+iy*S+ix]*255.f, fg=fake[1*hw+iy*S+ix]*255.f, fb=fake[2*hw+iy*S+ix]*255.f;
        uint8_t *p=rgb+((size_t)y*w+x)*3;
        p[0]=(uint8_t)(fr*m+p[0]*(1-m)); p[1]=(uint8_t)(fg*m+p[1]*(1-m)); p[2]=(uint8_t)(fb*m+p[2]*(1-m));
    }
}

/* Load the 512x512 inswapper projection ("buff2fs") from the .onnx protobuf. It's stored as the
 * initializer's packed float_data (TensorProto field 4, wire type 2 → tag 0x22) of exactly
 * 512*512*4 bytes, immediately followed by the name field (0x42 len 7 "buff2fs"). Scan for that. */
static int load_emap(const char *path, float *emap) {
    FILE *f=fopen(path,"rb"); if(!f) return -1;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if (sz<=0){ fclose(f); return -1; }
    uint8_t *buf=malloc((size_t)sz); if(!buf){fclose(f);return -1;}
    if (fread(buf,1,(size_t)sz,f)!=(size_t)sz){ free(buf); fclose(f); return -1; }
    fclose(f);
    const size_t need=(size_t)512*512*4;
    int rc=-1;
    for (long j=0;j+5<sz && rc!=0;j++){
        if (buf[j]!=0x22) continue;                 /* field 4 (float_data), wire type 2 (packed) */
        size_t len=0; int shift=0; long k=j+1;
        while (k<sz){ uint8_t b=buf[k++]; len|=(size_t)(b&0x7f)<<shift; if(!(b&0x80))break; shift+=7; if(shift>35){len=0;break;} }
        if (len!=need || k+(long)need>sz) continue;
        /* verify the "buff2fs" name follows this blob (disambiguates from any other 1 MB float array) */
        long after=k+(long)need;
        if (after+9<=sz && memcmp(buf+after,"\x42\x07""buff2fs",9)==0) { memcpy(emap,buf+k,need); rc=0; }
    }
    free(buf);
    return rc;
}

/* ---- EP selection (mirrors depth_onnx) ---- */
static void select_ep(bsdr_faceswap *fs, int use_gpu) {
    const OrtApi *o=fs->ort; fs->ep="cpu";
    if (use_gpu) {
#if defined(__ANDROID__)
        /* NNAPI uses a dedicated factory, not the string-name EP API; resolve it at runtime so a
         * build without it just falls through to CPU. */
        typedef OrtStatus *(*nnapi_fn)(OrtSessionOptions *, uint32_t);
        nnapi_fn nn = (nnapi_fn)dlsym(RTLD_DEFAULT, "OrtSessionOptionsAppendExecutionProvider_Nnapi");
        if (nn) { OrtStatus *s = nn(fs->opts, 0); if (!s) { fs->ep = "nnapi"; return; } o->ReleaseStatus(s); }
#endif
        const char *g=NULL;
#if defined(__APPLE__)
        g="CoreML";
#endif
        if (g){ OrtStatus *s=o->SessionOptionsAppendExecutionProvider(fs->opts,g,NULL,NULL,0); if(!s){fs->ep=g;return;} o->ReleaseStatus(s); }
#if defined(_WIN32) && defined(BSDR_ONNX_DML)
        { OrtStatus *s=OrtSessionOptionsAppendExecutionProvider_DML(fs->opts,0); if(!s){fs->ep="dml";return;} o->ReleaseStatus(s); }
#endif
#if defined(BSDR_ONNX_CUDA)
        { OrtCUDAProviderOptions c; memset(&c,0,sizeof c); OrtStatus *s=o->SessionOptionsAppendExecutionProvider_CUDA(fs->opts,&c); if(!s){fs->ep="cuda";return;} o->ReleaseStatus(s); }
#endif
    }
}

static OrtSession *open_sess(bsdr_faceswap *fs, const char *dir, const char *file) {
    char path[1024]; snprintf(path,sizeof path,"%s/%s",dir,file);
    OrtSession *s=NULL;
#ifdef _WIN32
    wchar_t wp[1024]; MultiByteToWideChar(CP_UTF8,0,path,-1,wp,1024);
    if (ob(fs->ort, fs->ort->CreateSession(fs->env,wp,fs->opts,&s),file)) return NULL;
#else
    if (ob(fs->ort, fs->ort->CreateSession(fs->env,path,fs->opts,&s),file)) return NULL;
#endif
    return s;
}

bsdr_faceswap *bsdr_faceswap_open(const char *model_dir, int use_gpu) {
    if (!model_dir) return NULL;
    bsdr_faceswap *fs=calloc(1,sizeof *fs);
    if (!fs) return NULL;
    fs->ort=OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!fs->ort){ free(fs); return NULL; }
    const OrtApi *o=fs->ort;
    OrtAllocator *al=NULL;
    if (ob(o,o->CreateEnv(ORT_LOGGING_LEVEL_WARNING,"bsdrfs",&fs->env),"CreateEnv")) goto fail;
    if (ob(o,o->CreateSessionOptions(&fs->opts),"opts")) goto fail;
    od(o,o->SetSessionGraphOptimizationLevel(fs->opts,ORT_ENABLE_ALL));
    select_ep(fs,use_gpu);
    fs->det=open_sess(fs,model_dir,"det_10g.onnx");
    fs->rec=open_sess(fs,model_dir,"w600k_r50.onnx");
    fs->swp=open_sess(fs,model_dir,"inswapper_128.onnx");
    if (!fs->det||!fs->rec||!fs->swp){ BSDR_WARN("bsdr.faceswap","missing model(s) in %s (need det_10g/w600k_r50/inswapper_128)",model_dir); goto fail; }
    if (ob(o,o->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&fs->mem),"mem")) goto fail;
    if (ob(o,o->GetAllocatorWithDefaultOptions(&al),"alloc")) goto fail;
    od(o,o->SessionGetInputName(fs->det,0,al,&fs->det_in));
    for (int i=0;i<9;i++) od(o,o->SessionGetOutputName(fs->det,i,al,&fs->det_out[i]));
    od(o,o->SessionGetInputName(fs->rec,0,al,&fs->rec_in));
    od(o,o->SessionGetOutputName(fs->rec,0,al,&fs->rec_out));
    /* inswapper inputs: index 0 = "target" (image), 1 = "source" (latent) — resolve by name */
    { char *a=NULL,*b=NULL; od(o,o->SessionGetInputName(fs->swp,0,al,&a)); od(o,o->SessionGetInputName(fs->swp,1,al,&b));
      if (a && strcmp(a,"source")==0){ fs->swp_in_s=a; fs->swp_in_t=b; } else { fs->swp_in_t=a; fs->swp_in_s=b; } }
    od(o,o->SessionGetOutputName(fs->swp,0,al,&fs->swp_out));
    { char emp[1024]; snprintf(emp,sizeof emp,"%s/inswapper_128.onnx",model_dir);
      fs->have_emap = (load_emap(emp,fs->emap)==0);
      if (!fs->have_emap) BSDR_WARN("bsdr.faceswap","could not extract emap from inswapper — swaps disabled"); }
    snprintf(fs->status,sizeof fs->status,"faceswap (%s)%s",fs->ep,fs->have_emap?"":" [no-emap]");
    BSDR_INFO("bsdr.faceswap","loaded det/rec/swap from %s (%s)",model_dir,fs->ep);
    return fs;
fail:
    bsdr_faceswap_close(fs);
    return NULL;
}

int bsdr_faceswap_set_source_rgb(bsdr_faceswap *fs, const uint8_t *rgb, int w, int h) {
    if (!fs||!rgb||!fs->have_emap) return -1;
    face_t faces[MAX_FACES]; int n=detect(fs,rgb,w,h,faces,MAX_FACES);
    if (n<=0){ BSDR_WARN("bsdr.faceswap","no face in source image"); return -1; }
    /* largest face */
    int best=0; float ba=0; for(int i=0;i<n;i++){ float a=(faces[i].x2-faces[i].x1)*(faces[i].y2-faces[i].y1); if(a>ba){ba=a;best=i;} }
    uint8_t al[112*112*3]; float M[6];
    align_face(rgb,w,h,&faces[best],112,al,M);
    float emb[512]; if (embed(fs,al,emb)!=0) return -1;
    /* latent = normed_emb (1x512) @ emap (512x512), then normalize */
    for (int j=0;j<512;j++){ float s=0; for(int i=0;i<512;i++) s+=emb[i]*fs->emap[i*512+j]; fs->latent[j]=s; }
    float nrm=0; for(int i=0;i<512;i++) nrm+=fs->latent[i]*fs->latent[i]; nrm=sqrtf(nrm)+1e-9f;
    for (int i=0;i<512;i++) fs->latent[i]/=nrm;
    fs->have_source=1;
    BSDR_INFO("bsdr.faceswap","source identity set (%d face(s) in image)",n);
    return 0;
}

int bsdr_faceswap_ready(const bsdr_faceswap *fs){ return fs && fs->have_source && fs->have_emap; }
const char *bsdr_faceswap_status(const bsdr_faceswap *fs){ return fs?fs->status:"unavailable"; }

int bsdr_faceswap_process_rgb(bsdr_faceswap *fs, uint8_t *rgb, int w, int h) {
    if (!fs||!rgb) return -1;
    if (!fs->have_source) return 0;
    const OrtApi *o=fs->ort;
    face_t faces[MAX_FACES]; int n=detect(fs,rgb,w,h,faces,MAX_FACES);
    int swapped=0;
    for (int i=0;i<n;i++){
        uint8_t al[128*128*3]; float M[6];
        align_face(rgb,w,h,&faces[i],128,al,M);
        float *blob=malloc(3*128*128*sizeof(float));
        int hw=128*128; for(int y=0;y<128;y++)for(int x=0;x<128;x++){ const uint8_t*p=al+((size_t)y*128+x)*3;
            blob[0*hw+y*128+x]=p[0]/255.f; blob[1*hw+y*128+x]=p[1]/255.f; blob[2*hw+y*128+x]=p[2]/255.f; }
        int64_t td[4]={1,3,128,128}, sd[2]={1,512};
        OrtValue *tv=mk_input(fs,blob,td,4), *sv=mk_input(fs,fs->latent,sd,2), *ov=NULL;
        const char *innames[2]={fs->swp_in_t,fs->swp_in_s}; const OrtValue *invals[2]={tv,sv};
        const char *outnames[1]={fs->swp_out};
        if (tv&&sv&&!ob(o,o->Run(fs->swp,NULL,innames,invals,2,outnames,1,&ov),"swap Run")){
            float *fake; od(o,o->GetTensorMutableData(ov,(void**)&fake));
            paste_back(rgb,w,h,fake,M); swapped++;
        }
        if (ov) o->ReleaseValue(ov);
        if (tv) o->ReleaseValue(tv);
        if (sv) o->ReleaseValue(sv);
        free(blob);
    }
    return swapped;
}

void bsdr_faceswap_close(bsdr_faceswap *fs) {
    if (!fs) return;
    const OrtApi *o=fs->ort;
    if (o){
        if (fs->det) o->ReleaseSession(fs->det);
        if (fs->rec) o->ReleaseSession(fs->rec);
        if (fs->swp) o->ReleaseSession(fs->swp);
        if (fs->mem) o->ReleaseMemoryInfo(fs->mem);
        if (fs->opts) o->ReleaseSessionOptions(fs->opts);
        if (fs->env) o->ReleaseEnv(fs->env);
    }
    free(fs);
}

#else  /* no ONNX Runtime */
bsdr_faceswap *bsdr_faceswap_open(const char *d,int g){ (void)d;(void)g; return NULL; }
int bsdr_faceswap_set_source_rgb(bsdr_faceswap *f,const uint8_t *r,int w,int h){ (void)f;(void)r;(void)w;(void)h; return -1; }
int bsdr_faceswap_ready(const bsdr_faceswap *f){ (void)f; return 0; }
int bsdr_faceswap_process_rgb(bsdr_faceswap *f,uint8_t *r,int w,int h){ (void)f;(void)r;(void)w;(void)h; return -1; }
const char *bsdr_faceswap_status(const bsdr_faceswap *f){ (void)f; return "unavailable (no ONNX)"; }
void bsdr_faceswap_close(bsdr_faceswap *f){ (void)f; }
#endif
