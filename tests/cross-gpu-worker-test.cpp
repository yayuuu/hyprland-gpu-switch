// Offscreen integration test: real FP16 DMA-BUF -> worker -> other GPU PQ10.
#include "../transfer/protocol.hpp"
#include "../transfer/gpu_route.hpp"
#include "../transfer/shared_output.hpp"
#include "../transfer/formats.hpp"
#include "../transfer/pq.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <vulkan/vulkan.h>
#include <cmath>
#include <iostream>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
using namespace Offload;
static void check(VkResult r,const char* what){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(what)+": "+std::to_string(r));}
struct Source {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};
    VkImage image{};VkDeviceMemory memory{};VkCommandPool pool{};VkCommandBuffer command{};
    VkSubresourceLayout layout{};uint64_t modifier=0;uint32_t format=DRM_FORMAT_ABGR16161616F;FD fd,acquire;std::optional<GPUDevice> pendingProducer;uint32_t family=0,external=VK_QUEUE_FAMILY_EXTERNAL;
    Source(const GPUDevice& gpu,bool tiled=false,bool alpha=false,uint32_t pixelFormat=DRM_FORMAT_ABGR16161616F) {
        format=pixelFormat;
        modifier=tiled?0x0100000000000001ULL:0;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;check(vkCreateInstance(&ci,nullptr,&instance),"instance");
        uint32_t n=0;vkEnumeratePhysicalDevices(instance,&n,nullptr);std::vector<VkPhysicalDevice> devices(n);vkEnumeratePhysicalDevices(instance,&n,devices.data());
        std::vector<const char*> extensions{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
        for(auto p:devices) {
            uint32_t count=0;vkEnumerateDeviceExtensionProperties(p,nullptr,&count,nullptr);std::vector<VkExtensionProperties> exts(count);vkEnumerateDeviceExtensionProperties(p,nullptr,&count,exts.data());
            auto has=[&](const char* name){return std::ranges::any_of(exts,[&](const auto& e){return std::strcmp(e.extensionName,name)==0;});};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
            VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
            if(has(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)){drm.pNext=props.pNext;props.pNext=&drm;}
            if(has(VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)){pci.pNext=props.pNext;props.pNext=&pci;}
            vkGetPhysicalDeviceProperties2(p,&props);
            if((drm.hasRender && uint32_t(drm.renderMajor)==gpu.renderMajor && uint32_t(drm.renderMinor)==gpu.renderMinor) || (has(VK_EXT_PCI_BUS_INFO_EXTENSION_NAME) && gpu.pci==std::array<uint32_t,4>{pci.pciDomain,pci.pciBus,pci.pciDevice,pci.pciFunction})) {
                physical=p;if(has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)){extensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);external=VK_QUEUE_FAMILY_FOREIGN_EXT;}break;
            }
        }
        if(!physical)throw std::runtime_error("Source GPU not found");
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
        for(;family<n;++family)if(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)break;
        if(family==n)throw std::runtime_error("No graphics queue");
        float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;dc.enabledExtensionCount=extensions.size();dc.ppEnabledExtensionNames=extensions.data();check(vkCreateDevice(physical,&dc,nullptr,&device),"device");vkGetDeviceQueue(device,family,0,&queue);
        VkImageDrmFormatModifierListCreateInfoEXT mods{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};mods.drmFormatModifierCount=1;mods.pDrmFormatModifiers=&modifier;
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};ext.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;ext.pNext=&mods;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};imageInfo.pNext=&ext;imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=transportFormat(format).value().vk;imageInfo.extent={64,32,1};imageInfo.mipLevels=1;imageInfo.arrayLayers=1;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;imageInfo.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;imageInfo.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        check(vkCreateImage(device,&imageInfo,nullptr,&image),"source image");
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,image,&req);VkPhysicalDeviceMemoryProperties mem{};vkGetPhysicalDeviceMemoryProperties(physical,&mem);
        uint32_t type=0;for(;type<mem.memoryTypeCount;++type)if((req.memoryTypeBits&(1u<<type)) && (mem.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))break;
        if(type==mem.memoryTypeCount)throw std::runtime_error("No host-visible source memory");
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};exportInfo.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=image;dedicated.pNext=&exportInfo;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.pNext=&dedicated;alloc.allocationSize=req.size;alloc.memoryTypeIndex=type;check(vkAllocateMemory(device,&alloc,nullptr,&memory),"source memory");check(vkBindImageMemory(device,image,memory,0),"bind source");
        VkImageSubresource sub{VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,0,0};vkGetImageSubresourceLayout(device,image,&sub,&layout);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;check(vkCreateCommandPool(device,&pi,nullptr,&pool),"pool");VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;check(vkAllocateCommandBuffers(device,&ca,&command),"commands");
        transition(false);
        check(vkResetCommandPool(device,pool,0),"reset clear pool");VkCommandBufferBeginInfo clearBegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(command,&clearBegin),"begin clear");
        VkClearColorValue clear{};clear.float32[0]=.25f;clear.float32[1]=.5f;clear.float32[2]=1;clear.float32[3]=alpha?.5f:1;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);check(vkEndCommandBuffer(command),"end clear");VkSubmitInfo clearSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};clearSubmit.commandBufferCount=1;clearSubmit.pCommandBuffers=&command;check(vkQueueSubmit(queue,1,&clearSubmit,VK_NULL_HANDLE),"clear source");check(vkQueueWaitIdle(queue),"clear complete");
        transition(true);
        VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};get.memory=memory;get.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        auto getFd=reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device,"vkGetMemoryFdKHR"));int out=-1;check(getFd(device,&get,&out),"export source");fd.reset(out);
    }
    void transition(bool release) {
        check(vkResetCommandPool(device,pool,0),"reset pool");VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(command,&begin),"begin");
        VkEvent event{};VkSemaphore semaphore{};
        if(release){
            VkEventCreateInfo eventInfo{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};check(vkCreateEvent(device,&eventInfo,nullptr,&event),"source wait event");
            vkCmdWaitEvents(command,1,&event,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,nullptr,0,nullptr,0,nullptr);
            VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};exportInfo.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};info.pNext=&exportInfo;check(vkCreateSemaphore(device,&info,nullptr,&semaphore),"source semaphore");
        }
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=release?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcQueueFamilyIndex=release?family:VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=release?external:VK_QUEUE_FAMILY_IGNORED;b.srcAccessMask=release?(VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT):0;b.dstAccessMask=release?0:VK_ACCESS_HOST_WRITE_BIT;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,0,nullptr,1,&b);check(vkEndCommandBuffer(command),"end");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;if(release){submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&semaphore;}
        check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE),"submit");
        if(release){
            VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};info.semaphore=semaphore;info.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            auto exportFd=reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device,"vkGetSemaphoreFdKHR"));int out=-1;check(exportFd(device,&info,&out),"source acquire fence");acquire.reset(out);pendingProducer=fenceProducer(acquire.value,gpuDevices());
            std::cout<<"Pending fence producer="<<(pendingProducer?pendingProducer->id:"unknown")<<'\n';
            check(vkSetEvent(device,event),"release source event");
        }
        check(vkQueueWaitIdle(queue),"source complete");
        if(semaphore)vkDestroySemaphore(device,semaphore,nullptr);
        if(event)vkDestroyEvent(device,event,nullptr);
    }
    ~Source(){if(device){vkDeviceWaitIdle(device);if(pool)vkDestroyCommandPool(device,pool,nullptr);if(image)vkDestroyImage(device,image,nullptr);if(memory)vkFreeMemory(device,memory,nullptr);vkDestroyDevice(device,nullptr);}if(instance)vkDestroyInstance(instance,nullptr);}
};
struct TargetGL {
    EGLDisplay display=EGL_NO_DISPLAY;EGLContext context=EGL_NO_CONTEXT;std::string renderer;
    explicit TargetGL(const char* node) {
        struct stat st{};if(stat(node,&st))throw std::runtime_error("Target render node unavailable");auto id=deviceID(st.st_rdev);
        auto query=reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));auto name=reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));auto platform=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if(!query || !name || !platform)throw std::runtime_error("EGL device enumeration unavailable");
        EGLint count=0;query(0,nullptr,&count);std::vector<EGLDeviceEXT> devices(count);query(count,devices.data(),&count);
        for(auto device:devices){auto path=name(device,EGL_DRM_RENDER_NODE_FILE_EXT);if(!path)path=name(device,EGL_DRM_DEVICE_FILE_EXT);if(!path || stat(path,&st) || deviceID(st.st_rdev)!=id)continue;display=platform(EGL_PLATFORM_DEVICE_EXT,device,nullptr);break;}
        EGLint major=0,minor=0;if(display==EGL_NO_DISPLAY || !eglInitialize(display,&major,&minor) || !eglBindAPI(EGL_OPENGL_ES_API))throw std::runtime_error("Target EGL unavailable");
        const EGLint attrs[]{EGL_CONTEXT_MAJOR_VERSION,3,EGL_CONTEXT_MINOR_VERSION,2,EGL_NONE};context=eglCreateContext(display,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,attrs);
        if(context==EGL_NO_CONTEXT || !eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context))throw std::runtime_error("Target GLES context unavailable");
        renderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    }
    ~TargetGL(){if(display!=EGL_NO_DISPLAY){eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);eglTerminate(display);}}
};
struct Child {
    FD socket;pid_t pid=0;
    explicit Child(const char* binary){int sockets[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET,0,sockets))throw std::runtime_error("socketpair");socket.reset(sockets[0]);pid=fork();if(!pid){close(sockets[0]);std::string fd=std::to_string(sockets[1]);execl(binary,binary,fd.c_str(),nullptr);_exit(127);}close(sockets[1]);if(pid<0)throw std::runtime_error("fork");}
    Packet receive(std::vector<FD>& fds){pollfd p{socket.value,POLLIN,0};if(poll(&p,1,10000)<=0)throw std::runtime_error("worker timeout");Packet result;if(!Offload::receive(socket.value,result,fds))throw std::runtime_error("worker disconnected");if(result.type==ERROR)throw std::runtime_error(result.error);return result;}
    ~Child(){socket.reset();if(pid>0){int status;while(waitpid(pid,&status,0)<0 && errno==EINTR){}}}
};
int main(int argc,char** argv){
    if(argc!=4 && argc!=5){std::cerr<<"usage: cross-gpu-worker-test WORKER SOURCE_RENDER_NODE TARGET_RENDER_NODE [copy|copy-tiled]\n";return 2;}
    try {
        auto all=gpuDevices();auto found=std::ranges::find_if(all,[&](const auto& d){return d.node==argv[2];});if(found==all.end())throw std::runtime_error("Unknown source node");auto gpu=*found;
        const bool copyOnly=argc==5;const bool tiled=copyOnly && std::string(argv[4]).find("tiled")!=std::string::npos;
        const bool eight=copyOnly && std::string(argv[4]).starts_with("copy8");
        const bool ten=copyOnly && std::string(argv[4]).starts_with("copy10");
        const uint32_t pixelFormat=eight?DRM_FORMAT_ABGR8888:ten?DRM_FORMAT_ABGR2101010:DRM_FORMAT_ABGR16161616F;
        Source source(gpu,tiled,copyOnly,pixelFormat);
        sync_file_info fenceInfo{};
        if(source.acquire.value>=0 && !ioctl(source.acquire.value,SYNC_IOC_FILE_INFO,&fenceInfo)) {
            std::vector<sync_fence_info> fences(fenceInfo.num_fences);fenceInfo.sync_fence_info=reinterpret_cast<uint64_t>(fences.data());
            if(!ioctl(source.acquire.value,SYNC_IOC_FILE_INFO,&fenceInfo))for(const auto& f:fences)std::cout<<"Source fence driver="<<f.driver_name<<" timeline="<<f.obj_name<<'\n';
        }
        auto identified=producerDevice(source.fd.value,getpid(),source.acquire.value);
        if(!identified)identified=source.pendingProducer;
        if(identified && identified->id!=gpu.id)throw std::runtime_error("DMA-BUF provenance mismatch");
        if(!identified)throw std::runtime_error("Source provenance unavailable even before fence completion");
        Child child(argv[1]);Packet hello;hello.type=HELLO;hello.width=64;hello.height=32;if(copyOnly)hello.fdFlags|=COPY_ONLY;hello.format=pixelFormat;hello.gpuMajor=gpu.renderMajor;hello.gpuMinor=gpu.renderMinor;hello.gpuPCI=gpu.pci;send(child.socket.value,hello);std::vector<FD> fds;auto ready=child.receive(fds);if(ready.type!=READY)throw std::runtime_error("Not ready");
        Packet job;job.type=CONVERT;job.width=64;job.height=32;job.format=pixelFormat;job.stride=source.layout.rowPitch;job.offset=source.layout.offset;job.source=1;job.serial=1;job.modifier=source.modifier;if(copyOnly)job.fdFlags|=COPY_ONLY;job.color[0]=job.color[5]=job.color[10]=job.color[13]=job.color[15]=1;
        std::vector<int> inputs{source.fd.value};if(source.acquire.value>=0){job.fdFlags|=ACQUIRE;inputs.push_back(source.acquire.value);}
        send(child.socket.value,job,inputs);auto output=child.receive(fds);if(output.type!=RESULT || fds.empty())throw std::runtime_error("Invalid result");
        if(output.fdFlags&COMPLETE){pollfd p{fds.at(1).value,POLLIN,0};if(poll(&p,1,10000)<=0)throw std::runtime_error("Conversion fence timeout");}
        TargetGL target(argv[3]);
        std::cout<<"Target external-only modifier: "<<externalOutput(target.display,output.format)<<'\n';
        GLuint texture=0;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexStorage2D(GL_TEXTURE_2D,1,copyOnly?transportFormat(pixelFormat).value().gl:GL_RGB10_A2,64,32);
        auto external=std::make_unique<SharedOutput>();external->import(64,32,output.format,fds[0].value,output.offset,output.stride);
        glViewport(2,3,7,9);glColorMask(GL_FALSE,GL_TRUE,GL_FALSE,GL_FALSE);glEnable(GL_SCISSOR_TEST);glEnable(GL_BLEND);glEnable(GL_RASTERIZER_DISCARD);glActiveTexture(GL_TEXTURE2);
        external->copy(texture);
        GLint viewport[4]{},active=0;GLboolean mask[4]{};glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetBooleanv(GL_COLOR_WRITEMASK,mask);
        if(viewport[0]!=2 || viewport[1]!=3 || viewport[2]!=7 || viewport[3]!=9 || active!=GL_TEXTURE2 || mask[0] || !mask[1] || mask[2] || mask[3] || !glIsEnabled(GL_SCISSOR_TEST) || !glIsEnabled(GL_BLEND) || !glIsEnabled(GL_RASTERIZER_DISCARD))throw std::runtime_error("GPU copy leaked GL state");
        GLuint fbo=0;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
        if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Output framebuffer incomplete");
        if(eight) {
            std::vector<uint8_t> pixels(64*32*4);glReadPixels(0,0,64,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("RGBA8 output readback failed");
            for(unsigned i=0;i<64*32;++i)for(unsigned c=0;c<4;++c)if(std::abs(int(pixels[i*4+c])-std::array<int,4>{64,128,255,128}[c])>1)throw std::runtime_error("RGBA8 transport altered color or alpha");
        }else if(ten) {
            std::vector<uint32_t> pixels(64*32);glReadPixels(0,0,64,32,GL_RGBA,GL_UNSIGNED_INT_2_10_10_10_REV,pixels.data());if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("RGB10 output readback failed");
            for(auto pixel:pixels){for(unsigned c=0;c<3;++c)if(std::abs(int((pixel>>(10*c))&1023)-std::array<int,3>{256,512,1023}[c])>1)throw std::runtime_error("RGB10 transport altered color");if((pixel>>30)!=2)throw std::runtime_error("RGB10 transport altered alpha");}
        }else if(copyOnly) {
            std::vector<float> pixels(64*32*4);glReadPixels(0,0,64,32,GL_RGBA,GL_FLOAT,pixels.data());if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("FP16 output readback failed");
            for(unsigned i=0;i<64*32;++i)for(unsigned c=0;c<4;++c)if(std::abs(pixels[i*4+c]-std::array<float,4>{.25f,.5f,1.f,.5f}[c])>0.000001f)throw std::runtime_error("FP16 transport altered color or alpha");
        }else {
        std::vector<uint32_t> pixels(64*32);glReadPixels(0,0,64,32,GL_RGBA,GL_UNSIGNED_INT_2_10_10_10_REV,pixels.data());if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("Output readback failed");
        for(unsigned i=0;i<64*32;++i){uint32_t pixel=pixels[i];for(unsigned c=0;c<3;++c){auto expected=std::lround(PQLut::exact(std::array<double,3>{.25,.5,1}[c])*1023);if(std::abs(long((pixel>>(c*10))&1023)-expected)>2)throw std::runtime_error("PQ pixel mismatch");}}
        }
        glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);external.reset();
        std::cout<<"PASS: "<<gpu.id<<(copyOnly?" native format + alpha -> native format -> ":" FP16 -> PQ10 -> ")<<target.renderer<<"; no PRIME flag; fence, pixels and GL state verified\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
