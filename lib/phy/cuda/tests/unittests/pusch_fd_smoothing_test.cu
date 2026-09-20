// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Explicit offline numerical/graph regression against an immutable CPU input.
#include "../../src/pusch_e2e_launch.cuh"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr int guard = 32;
constexpr std::array<float, 15> coefficients = {
    -.0391585909F, -.0287990728F, 0.F, .0445245383F, .0970724536F,
    .1467045930F, .1821531663F, .1950058248F, .1821531663F,
    .1467045930F, .0970724536F, .0445245383F, 0.F, -.0287990728F, -.0391585909F};
struct geometry { int mode, prbs, ports, layers, dmrs, type; };
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void checked(cudaError_t status) { require(status == cudaSuccess, cudaGetErrorString(status)); }
float half(float value) { return __half2float(__float2half_rn(value)); }
int pilots(const geometry& g) { return g.type == 2 ? 4 : 6; }
int coordinate(const geometry& g, int layer, int p) {
    const int group = layer/2;
    return g.type == 2 ? 2*group+(p%2)+6*(p/2) : group+2*p;
}
size_t index(const geometry& g, int symbol, int prb, int port, int layer, int sc) {
    if (g.mode == 3) return ((symbol*g.ports+port)*g.prbs+prb)*12+sc;
    return ((((symbol*g.prbs+prb)*g.ports+port)*g.layers+layer)*12)+sc;
}
std::vector<float2> reference(const geometry& g, const std::vector<float2>& input) {
    auto output = input;
    for (int d=0; d<g.dmrs; ++d) for (int port=0; port<g.ports; ++port)
      for (int layer=0; layer<g.layers; ++layer) for (int rb=0; rb<g.prbs; ++rb) {
        std::array<float2, 6> filtered{};
        for (int p=0; p<pilots(g); ++p) {
            float x=0, y=0, norm=0;
            for (int tap=0; tap<15; ++tap) {
                const int source=rb*pilots(g)+p+tap-7;
                if (source<0 || source>=g.prbs*pilots(g)) continue;
                const auto value=input[index(g,d,source/pilots(g),port,layer,coordinate(g,layer,source%pilots(g)))];
                x=std::fma(coefficients[tap],value.x,x); y=std::fma(coefficients[tap],value.y,y); norm+=coefficients[tap];
            }
            x*=1.F/norm; y*=1.F/norm;
            filtered[p]=make_float2(g.mode==1 || g.mode==2 ? half(x):x, g.mode==1 || g.mode==2 ? half(y):y);
        }
        for (int sc=0; sc<12; ++sc) {
            int lo=0, hi=0;
            while (lo+1<pilots(g) && coordinate(g,layer,lo+1)<=sc) ++lo;
            hi=std::min(lo+1,pilots(g)-1);
            if (sc<=coordinate(g,layer,lo) || lo==hi) hi=lo;
            float alpha=lo==hi ? 0.F : float(sc-coordinate(g,layer,lo))/float(coordinate(g,layer,hi)-coordinate(g,layer,lo));
            float2 out{};
            if (g.mode==1 || g.mode==2) {
                out.x=half(half(filtered[lo].x*half(1-alpha))+half(filtered[hi].x*half(alpha)));
                out.y=half(half(filtered[lo].y*half(1-alpha))+half(filtered[hi].y*half(alpha)));
            } else {
                out.x=(1-alpha)*filtered[lo].x+alpha*filtered[hi].x;
                out.y=(1-alpha)*filtered[lo].y+alpha*filtered[hi].y;
                if (g.mode==0) { out.x=half(out.x); out.y=half(out.y); }
            }
            output[index(g,d,rb,port,layer,sc)]=out;
        }
    }
    return output;
}
struct resources {
    void* storage{}; cudaStream_t stream{}; cudaEvent_t begin{},end{};
    cudaGraph_t graph{}; cudaGraphExec_t executable{};
    void clear_graph() {
        if (executable) { checked(cudaGraphExecDestroy(executable)); executable=nullptr; }
        if (graph) { checked(cudaGraphDestroy(graph)); graph=nullptr; }
    }
    ~resources() {
        if (stream && cudaStreamSynchronize(stream)!=cudaSuccess) std::_Exit(5);
        if (executable) cudaGraphExecDestroy(executable);
        if (graph) cudaGraphDestroy(graph);
        if (storage) cudaFree(storage);
        if (begin) cudaEventDestroy(begin);
        if (end) cudaEventDestroy(end);
        if (stream) cudaStreamDestroy(stream);
    }
};
void launch(resources& r, const geometry& g) {
    if (g.mode==0) launch_kernel_fd_smoothing_fp16(dim3(g.prbs,g.dmrs),g.ports,0,r.stream,
        static_cast<__half2*>(r.storage)+guard,g.prbs,g.ports,g.dmrs,g.type);
    else if (g.mode==1) launch_kernel_mimo_fd_smoothing_fp16(dim3(g.prbs,g.dmrs,g.layers),g.ports,0,r.stream,
        static_cast<__half2*>(r.storage)+guard,g.prbs,g.ports,g.layers,g.dmrs,g.type);
    else if (g.mode==2) launch_kernel_mimo_fd_smoothing_fp16_wide(r.stream,
        static_cast<__half2*>(r.storage)+guard,g.prbs,g.ports,g.layers,g.dmrs,g.type);
    else launch_kernel_fd_smoothing_fp32(dim3(g.prbs,g.dmrs),g.ports,0,r.stream,
        static_cast<cuFloatComplex*>(r.storage)+guard,g.prbs,g.ports,g.dmrs);
    checked(cudaGetLastError());
}
void cold_check() {
    geometry g{0,4,1,1,2,1};
    std::vector<float2> constant(4*12*2,make_float2(.5F,-.75F));
    const auto out=reference(g,constant);
    for (auto v:out) require(v.x==.5F && v.y==-.75F,"constant-channel reference");
    require(index({3,4,2,1,2,1},1,3,1,0,11)==191,"FP32 layout");
    require(coordinate({1,4,4,4,2,2},3,3)==9,"type-2 CDM layout");
}
struct measurements { std::string json; bool passed; };
measurements execute() {
    resources r; int least{},greatest{};
    checked(cudaDeviceGetStreamPriorityRange(&least,&greatest));
    checked(cudaStreamCreateWithPriority(&r.stream,cudaStreamNonBlocking,least));
    checked(cudaEventCreate(&r.begin)); checked(cudaEventCreate(&r.end));
    checked(cudaMalloc(&r.storage,(size_t(4)*273*8*4*12+2*guard)*8));
    std::ostringstream rows;
    uint64_t total_errors=0, nonrepeatable=0; unsigned cases=0; float largest=0;
    for (int mode:{0,1,2,3}) for (int width:{1,4,51,273}) for (int type:{1,2}) {
      if (mode==3 && type==2) continue;
      for (int ports:{1,2,4,8}) for (int symbols:{1,2,4}) {
        const int layers=mode==1 || mode==2 ? (ports==1 ? 1 : ports==2 ? 2 : ports==4 ? 3 : 4) : 1;
        geometry g{mode,width,ports,layers,symbols,type};
        const size_t n=size_t(width)*ports*layers*symbols*12;
        const size_t element=mode==3 ? 8:4;
        std::vector<float2> input(n);
        std::vector<uint8_t> packed((n+2*guard)*element,0x5a),result(packed.size()),first;
        for (size_t i=0;i<n;++i) {
            const int s=int(i%997);
            float x=.2F+std::sin(s*.371F)*.8F, y=-.3F+std::cos(s*.291F)*.7F;
            if (mode!=3) { x=half(x); y=half(y); }
            input[i]=make_float2(x,y);
            if (mode==3) std::memcpy(packed.data()+(i+guard)*8,&input[i],8);
            else { const __half2 value=__floats2half2_rn(x,y); std::memcpy(packed.data()+(i+guard)*4,&value,4); }
        }
        const auto expected=reference(g,input);
        // A captured replay must preserve the same snapshot semantics.
        checked(cudaStreamBeginCapture(r.stream,cudaStreamCaptureModeThreadLocal));
        launch(r,g); checked(cudaStreamEndCapture(r.stream,&r.graph));
        checked(cudaGraphInstantiate(&r.executable,r.graph,nullptr,nullptr,0));
        std::array<float,3> elapsed{}; uint64_t errors=0; float maximum=0;
        for (unsigned repeat=0;repeat<3;++repeat) {
            checked(cudaMemcpyAsync(r.storage,packed.data(),packed.size(),cudaMemcpyHostToDevice,r.stream));
            checked(cudaEventRecord(r.begin,r.stream));
            if (repeat==2) checked(cudaGraphLaunch(r.executable,r.stream)); else launch(r,g);
            checked(cudaEventRecord(r.end,r.stream));
            checked(cudaMemcpyAsync(result.data(),r.storage,result.size(),cudaMemcpyDeviceToHost,r.stream));
            checked(cudaStreamSynchronize(r.stream));
            checked(cudaEventElapsedTime(&elapsed[repeat],r.begin,r.end)); elapsed[repeat]*=1000;
            for (size_t j=0;j<guard*element;++j) require(result[j]==0x5a && result[result.size()-1-j]==0x5a,"guard overwrite");
            if (repeat==0) first=result; else if (result!=first) ++nonrepeatable;
            for (size_t i=0;i<n;++i) {
                float2 actual{};
                if (mode==3) std::memcpy(&actual,result.data()+(i+guard)*8,8);
                else { __half2 value{}; std::memcpy(&value,result.data()+(i+guard)*4,4); actual=__half22float2(value); }
                const float error=std::max(std::abs(actual.x-expected[i].x),std::abs(actual.y-expected[i].y));
                require(std::isfinite(actual.x) && std::isfinite(actual.y),"nonfinite output");
                maximum=std::max(maximum,error);
                const float tolerance=(mode==3 ? 2e-6F:1e-3F)*(1+std::max(std::abs(expected[i].x),std::abs(expected[i].y)));
                errors+=error>tolerance;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        r.clear_graph(); total_errors+=errors; largest=std::max(largest,maximum);
        if (cases++) rows<<",";
        rows<<"{\"mode\":"<<mode<<",\"prbs\":"<<width<<",\"ports\":"<<ports<<",\"layers\":"<<layers
            <<",\"dmrs\":"<<symbols<<",\"type\":"<<type<<",\"errors\":"<<errors<<",\"max_error\":"<<maximum
            <<",\"gpu_us\":["<<elapsed[0]<<","<<elapsed[1]<<","<<elapsed[2]<<"]}";
      }
    }
    std::ostringstream report;
    report<<"{\"cases\":"<<cases<<",\"executions\":"<<cases*3<<",\"mismatched_samples\":"<<total_errors
          <<",\"nonrepeatable_runs\":"<<nonrepeatable<<",\"max_error\":"<<largest<<",\"rows\":["<<rows.str()<<"]}";
    return {report.str(), total_errors==0 && nonrepeatable==0};
}
}
int main(int argc,char** argv) {
    try {
        cold_check();
        if (argc==1) { std::puts("FD_SMOOTHING_COLD_OK"); return 0; }
        const bool characterize=argc==4 && !std::strcmp(argv[1],"--characterize");
        require(argc==4 && (characterize || !std::strcmp(argv[1],"--execute")) && !std::strcmp(argv[2],"--output"),
                "usage: --execute|--characterize --output NEW_JSON (GPU checks are explicit)");
        const int fd=open(argv[3],O_WRONLY|O_CREAT|O_EXCL,0600);
        require(fd>=0,"new output path required");
        FILE* file=fdopen(fd,"w"); if (!file) { close(fd); throw std::runtime_error("output stream"); }
        try {
            const auto report=execute();
            const bool wrote=std::fwrite(report.json.data(),1,report.json.size(),file)==report.json.size();
            const int closed=std::fclose(file); file=nullptr; require(wrote && closed==0,"report write");
            require(characterize || report.passed,"snapshot numerical/determinism regression; see report");
            std::puts(characterize ? "FD_SMOOTHING_CHARACTERIZATION_COMPLETE" : "FD_SMOOTHING_GPU_OK");
        } catch (...) { if(file) std::fclose(file); throw; }
        // Only --characterize may succeed with recorded numerical mismatches.
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 2; }
}
