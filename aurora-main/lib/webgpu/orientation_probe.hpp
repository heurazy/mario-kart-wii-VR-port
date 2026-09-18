#pragma once
#include <webgpu/webgpu_cpp.h>
#include <array>
#include <atomic>
#include <memory>

namespace aurora::webgpu {
struct OutputOrientation {
  bool valid = false;
  bool needsCopy = false;
  int copyMode = 0;
};

// No scene analysis: an asymmetric, offscreen calibration image gives a known
// answer independent of headset roll, game cameras, or the current race.
inline OutputOrientation probe_output_orientation(wgpu::Instance instance, wgpu::Device device,
    wgpu::TextureFormat format, const std::array<wgpu::RenderPipeline,4>& copies) {
  if(format != wgpu::TextureFormat::RGBA8Unorm && format != wgpu::TextureFormat::BGRA8Unorm)
    return {};
  constexpr uint32_t width=16, height=8, stride=256;
  const bool bgra=format==wgpu::TextureFormat::BGRA8Unorm;
  const wgpu::TextureDescriptor td{.usage=wgpu::TextureUsage::RenderAttachment|
    wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopySrc,
    .size={width,height,1},.format=format};
  auto source=device.CreateTexture(&td), target=device.CreateTexture(&td);
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code=R"(
    struct V { @builtin(position) p:vec4f, @location(0) marker:vec2f };
    @vertex fn vs(@builtin(vertex_index) i:u32)->V {
      let xy=array<vec2f,3>(vec2f(-1,1),vec2f(-1,-3),vec2f(3,1));
      var v:V;v.p=vec4f(xy[i],0,1);v.marker=xy[i];return v;
    }
    @fragment fn fs(v:V)->@location(0) vec4f {
      let quadrant=1u+select(0u,1u,v.marker.x>0)+select(0u,2u,v.marker.y<0);
      return vec4f(f32(quadrant)*0.2,0.0,0.0,1.0);
    }
  )";
  const wgpu::ShaderModuleDescriptor md{.nextInChain=&wgsl};
  auto shader=device.CreateShaderModule(&md);
  const wgpu::ColorTargetState color{.format=format};
  const wgpu::FragmentState fragment{.module=shader,.entryPoint="fs",.targetCount=1,.targets=&color};
  wgpu::RenderPipelineDescriptor pd{};pd.vertex={.module=shader,.entryPoint="vs"};pd.fragment=&fragment;
  auto raster=device.CreateRenderPipeline(&pd);
  auto encode=[&](wgpu::CommandEncoder encoder,wgpu::Texture texture,wgpu::RenderPipeline pipeline,wgpu::BindGroup binding) {
    const wgpu::RenderPassColorAttachment attachment{.view=texture.CreateView(),.loadOp=wgpu::LoadOp::Clear,.storeOp=wgpu::StoreOp::Store};
    const wgpu::RenderPassDescriptor passDesc{.colorAttachmentCount=1,.colorAttachments=&attachment};
    auto pass=encoder.BeginRenderPass(&passDesc);pass.SetPipeline(pipeline);
    if(binding) pass.SetBindGroup(0,binding);
    pass.Draw(3);pass.End();
  };
  auto read=[&](wgpu::CommandEncoder encoder,wgpu::Texture texture)->int {
    const wgpu::BufferDescriptor bd{.usage=wgpu::BufferUsage::CopyDst|wgpu::BufferUsage::MapRead,.size=stride*height};
    auto buffer=device.CreateBuffer(&bd);
    const wgpu::TexelCopyTextureInfo src{.texture=texture};
    const wgpu::TexelCopyBufferInfo dst{.layout={.bytesPerRow=stride,.rowsPerImage=height},.buffer=buffer};
    const wgpu::Extent3D extent{width,height,1};encoder.CopyTextureToBuffer(&src,&dst,&extent);
    auto commands=encoder.Finish();device.GetQueue().Submit(1,&commands);
    // Shared callback state remains alive even if the bounded wait times out.
    auto mapped=std::make_shared<std::atomic<bool>>(false);
    const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,stride*height,wgpu::CallbackMode::WaitAnyOnly,
      [mapped](wgpu::MapAsyncStatus status,wgpu::StringView) {mapped->store(status==wgpu::MapAsyncStatus::Success);});
    if(instance.WaitAny(future,1000000000)!=wgpu::WaitStatus::Success||!mapped->load()) return -1;
    const auto* data=static_cast<const unsigned char*>(buffer.GetConstMappedRange());
    int detected=-1;
    for(int mode=0;mode<4;++mode) {
      bool matches=true;
      for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
        const auto sx=mode>=2?width-1-x:x;
        const auto sy=mode==1||mode==2?height-1-y:y;
        const int expected=51*(1+(sx>=width/2?1:0)+(sy>=height/2?2:0));
        const auto pixel=data+y*stride+x*4;
        const int red=pixel[bgra?2:0];
        matches &= red>=expected-1 && red<=expected+1 && pixel[1]==0 && pixel[3]==255;
      }
      if(matches) detected=mode;
    }
    buffer.Unmap();return detected;
  };
  auto encoder=device.CreateCommandEncoder();encode(encoder,source,raster,{});
  const int raw=read(encoder,source);
  if(raw<0) return {};
  auto sampler=device.CreateSampler();
  const std::array entries{wgpu::BindGroupEntry{.binding=0,.sampler=sampler},
    wgpu::BindGroupEntry{.binding=1,.textureView=source.CreateView()}};
  for(int mode=0;mode<4;++mode) {
    const wgpu::BindGroupDescriptor bd{.layout=copies[mode].GetBindGroupLayout(0),.entryCount=entries.size(),.entries=entries.data()};
    auto binding=device.CreateBindGroup(&bd);
    encoder=device.CreateCommandEncoder();encode(encoder,target,copies[mode],binding);
    const int result=read(encoder,target);
    if(result<0) return {};
    if(result==0) return {true,raw!=0,mode};
  }
  return {};
}
} // namespace aurora::webgpu
