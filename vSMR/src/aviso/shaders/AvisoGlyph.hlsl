cbuffer View : register(b0) { float2 inverseViewport; float2 padding; };
Buffer<float4> Triangles : register(t0);
Buffer<float4> Styles : register(t1);
Buffer<uint4> Bins : register(t2);
Buffer<uint> TriangleIds : register(t3);
struct Input { float2 position:POSITION; float2 local:TEXCOORD0; uint style:TEXCOORD1; };
struct Output { float4 position:SV_POSITION; noperspective float2 local:TEXCOORD0; nointerpolation uint style:TEXCOORD1; };
Output VS(Input v) { Output o; o.position=float4(v.position.x*inverseViewport.x-1,1-v.position.y*inverseViewport.y,0,1); o.local=v.local; o.style=v.style; return o; }
void Clip(inout float2 polygon[8], inout uint count, float2 normal)
{
    float2 result[8]; [unroll] for(uint init=0;init<8;++init) result[init]=0; uint resultCount=0;
    if(count==0) return;
    float2 previous=polygon[count-1]; float previousDistance=dot(previous,normal)-0.5;
    [loop] for(uint i=0;i<count;++i)
    {
        float2 current=polygon[i]; float currentDistance=dot(current,normal)-0.5;
        bool previousInside=previousDistance<=0, currentInside=currentDistance<=0;
        if(previousInside != currentInside && resultCount<8)
            result[resultCount++]=previous+(current-previous)*(previousDistance/(previousDistance-currentDistance));
        if(currentInside && resultCount<8) result[resultCount++]=current;
        previous=current; previousDistance=currentDistance;
    }
    count=resultCount;
    [unroll] for(uint j=0;j<8;++j) if(j<count) polygon[j]=result[j];
}
float Coverage(uint start, uint count, float2 pixel)
{
    float coverage=0;
    [loop] for(uint i=0;i<count;++i)
    {
        uint index=TriangleIds[start+i]*3; float4 bounds=Triangles[index];
        if(bounds.z <= pixel.x-0.5 || bounds.x >= pixel.x+0.5 || bounds.w <= pixel.y-0.5 || bounds.y >= pixel.y+0.5) continue;
        float4 a=Triangles[index+1], b=Triangles[index+2];
        float2 polygon[8]; [unroll] for(uint init=0;init<8;++init) polygon[init]=0;
        polygon[0]=a.xy-pixel; polygon[1]=a.zw-pixel; polygon[2]=b.xy-pixel;
        uint n=3; Clip(polygon,n,float2(1,0)); Clip(polygon,n,float2(-1,0)); Clip(polygon,n,float2(0,1)); Clip(polygon,n,float2(0,-1));
        if(n<3) continue;
        float twiceArea=0;float2 previous=polygon[n-1];
        [loop] for(uint j=0;j<n;++j){float2 current=polygon[j];twiceArea += previous.x*current.y-previous.y*current.x;previous=current;}
        coverage += abs(twiceArea)*0.5;
    }
    return saturate(coverage);
}
float4 PS(Output v):SV_TARGET
{
    uint base=v.style*4;float4 grid=Styles[base];
    int2 bin=(int2)floor((v.local-grid.xy)/4);
    if(any(bin<0)||bin.x>=grid.z||bin.y>=grid.w) return 0;
    uint4 ranges=Bins[(uint)Styles[base+1].x+(uint)bin.y*(uint)grid.z+(uint)bin.x];
    float4 text=Styles[base+2],halo=Styles[base+3];
    float fillCoverage=text.a>0 ? Coverage((uint)ranges.x,(uint)ranges.y,v.local) : 0;
    float haloCoverage=halo.a>0 ? Coverage((uint)ranges.z,(uint)ranges.w,v.local) : 0;
    float4 core=text*fillCoverage, behind=halo*haloCoverage;
    return core+behind*(1-core.a);
}
