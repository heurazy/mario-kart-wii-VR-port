#include <webgpu/webgpu_cpp.h>
#include "../lib/webgpu/orientation_probe.hpp"
#include <array>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

// Exercise the production copy shader, including its specialization constants,
// using asymmetric pixels and actual D3D12 readback, without a headset.
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ifstream file(argv[1]);
  const std::string cpp{std::istreambuf_iterator<char>(file),{}};
  const auto begin=cpp.find("sourceDescriptor.code = R\"\"\"(");
  if(begin==std::string::npos) return 2;
  const auto start=cpp.find('(',begin)+1, end=cpp.find(")\"\"\";",start);
  if(end==std::string::npos) return 2;
  const auto code=cpp.substr(start,end-start);
  static std::atomic<int> errors{0};
  const auto timed=wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor id{};id.requiredFeatureCount=1;id.requiredFeatures=&timed;
  auto instance=wgpu::CreateInstance(&id);
  wgpu::Adapter adapter;
  wgpu::RequestAdapterOptions options{.backendType=wgpu::BackendType::D3D12};
  auto future=instance.RequestAdapter(&options,wgpu::CallbackMode::WaitAnyOnly,
    [&](wgpu::RequestAdapterStatus status,wgpu::Adapter a,wgpu::StringView) {
      if(status==wgpu::RequestAdapterStatus::Success) adapter=std::move(a);
    });
  if(instance.WaitAny(future,5000000000)!=wgpu::WaitStatus::Success||!adapter) return 2;
  wgpu::Device device;
  wgpu::DeviceDescriptor dd{};
  dd.SetUncapturedErrorCallback([](const wgpu::Device&,wgpu::ErrorType,wgpu::StringView text) {
    ++errors;std::cerr<<std::string_view(text)<<'\n';
  });
  future=adapter.RequestDevice(&dd,wgpu::CallbackMode::WaitAnyOnly,
    [&](wgpu::RequestDeviceStatus status,wgpu::Device d,wgpu::StringView) {
      if(status==wgpu::RequestDeviceStatus::Success) device=std::move(d);
    });
  if(instance.WaitAny(future,5000000000)!=wgpu::WaitStatus::Success||!device) return 2;
  wgpu::ShaderSourceWGSL wgsl{};wgsl.code=code.c_str();
  wgpu::ShaderModuleDescriptor md{};md.nextInChain=&wgsl;
  auto shader=device.CreateShaderModule(&md);
  const wgpu::ColorTargetState color{.format=wgpu::TextureFormat::RGBA8Unorm};
  const wgpu::FragmentState fragment{.module=shader,.entryPoint="fs_main",.targetCount=1,.targets=&color};
  const wgpu::TextureDescriptor inputDesc{.usage=wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopyDst,
    .size={4,4,1},.format=wgpu::TextureFormat::RGBA8Unorm};
  auto input=device.CreateTexture(&inputDesc);
  std::array<unsigned char,64> pixels{};
  for(int i=0;i<16;++i) {pixels[i*4]=static_cast<unsigned char>(i*13);pixels[i*4+3]=255;}
  const wgpu::TexelCopyTextureInfo inputCopy{.texture=input};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow=16,.rowsPerImage=4};
  const wgpu::Extent3D inputSize{4,4,1};
  device.GetQueue().WriteTexture(&inputCopy,pixels.data(),pixels.size(),&layout,&inputSize);
  auto sampler=device.CreateSampler();
  std::array<wgpu::RenderPipeline,4> copies;
  int checked=0;
  for(int mode=0;mode<4;++mode) for(uint32_t scale : {1u,2u}) for(int eye=0;eye<2;++eye) {
    const std::array constants{wgpu::ConstantEntry{.key="flip_x",.value=mode>=2?1.0:0.0},
      wgpu::ConstantEntry{.key="flip_y",.value=mode==1||mode==2?1.0:0.0}};
    wgpu::RenderPipelineDescriptor pd{};
    pd.vertex={.module=shader,.entryPoint="vs_main",.constantCount=constants.size(),.constants=constants.data()};
    pd.fragment=&fragment;
    auto pipeline=device.CreateRenderPipeline(&pd);
    copies[mode]=pipeline;
    const std::array entries{wgpu::BindGroupEntry{.binding=0,.sampler=sampler},
      wgpu::BindGroupEntry{.binding=1,.textureView=input.CreateView()}};
    const wgpu::BindGroupDescriptor bgd{.layout=pipeline.GetBindGroupLayout(0),.entryCount=entries.size(),.entries=entries.data()};
    auto binding=device.CreateBindGroup(&bgd);
    const uint32_t size=4*scale;
    const wgpu::TextureDescriptor outDesc{.usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::CopySrc,
      .size={size,size,1},.format=wgpu::TextureFormat::RGBA8Unorm};
    auto output=device.CreateTexture(&outDesc);
    auto encoder=device.CreateCommandEncoder();
    const wgpu::RenderPassColorAttachment attachment{.view=output.CreateView(),.loadOp=wgpu::LoadOp::Clear,.storeOp=wgpu::StoreOp::Store};
    const wgpu::RenderPassDescriptor passDesc{.colorAttachmentCount=1,.colorAttachments=&attachment};
    auto pass=encoder.BeginRenderPass(&passDesc);
    pass.SetPipeline(pipeline);pass.SetBindGroup(0,binding);pass.Draw(3);pass.End();
    const wgpu::BufferDescriptor bd{.usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=256*size};
    auto buffer=device.CreateBuffer(&bd);
    const wgpu::TexelCopyTextureInfo src{.texture=output};
    const wgpu::TexelCopyBufferInfo dst{.layout={.bytesPerRow=256,.rowsPerImage=size},.buffer=buffer};
    const wgpu::Extent3D extent{size,size,1};encoder.CopyTextureToBuffer(&src,&dst,&extent);
    auto command=encoder.Finish();device.GetQueue().Submit(1,&command);
    bool mapped=false;
    future=buffer.MapAsync(wgpu::MapMode::Read,0,256*size,wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::MapAsyncStatus status,wgpu::StringView) {mapped=status==wgpu::MapAsyncStatus::Success;});
    if(instance.WaitAny(future,5000000000)!=wgpu::WaitStatus::Success||!mapped) return 2;
    const auto* result=static_cast<const unsigned char*>(buffer.GetConstMappedRange());
    for(uint32_t y=0;y<size;++y) for(uint32_t x=0;x<size;++x) {
      const auto sx=mode>=2?3-x/scale:x/scale;
      const auto sy=mode==1||mode==2?3-y/scale:y/scale;
      if(result[y*256+x*4]!=pixels[(sy*4+sx)*4]) ++errors;
    }
    buffer.Unmap();++checked;
  }
  for(auto format : {wgpu::TextureFormat::RGBA8Unorm,wgpu::TextureFormat::BGRA8Unorm}) {
    const wgpu::ColorTargetState probeColor{.format=format};
    const wgpu::FragmentState probeFragment{.module=shader,.entryPoint="fs_main",.targetCount=1,.targets=&probeColor};
    for(int mode=0;mode<4;++mode) {
      const std::array constants{wgpu::ConstantEntry{.key="flip_x",.value=mode>=2?1.0:0.0},
        wgpu::ConstantEntry{.key="flip_y",.value=mode==1||mode==2?1.0:0.0}};
      wgpu::RenderPipelineDescriptor pd{};
      pd.vertex={.module=shader,.entryPoint="vs_main",.constantCount=constants.size(),.constants=constants.data()};
      pd.fragment=&probeFragment;copies[mode]=device.CreateRenderPipeline(&pd);
    }
    for(int injection=0;injection<4;++injection) {
    auto candidates=copies;
    // Simulate a driver/backend changing the default output transform. The
    // detector must choose the measured upright pipeline without a preference.
    std::swap(candidates[0],candidates[injection]);
    const auto result=aurora::webgpu::probe_output_orientation(instance,device,format,candidates);
    if(!result.valid || result.needsCopy || result.copyMode!=injection) ++errors;
    ++checked;
    }
  }
  std::cout<<checked<<" D3D12 orientation and automatic detection cases, "<<errors<<" errors\n";
  device.Destroy();return errors?1:0;
}
