#include <volk.h>
#include <iostream>
#include <vector>

int main() {
    if(volkInitialize()!=VK_SUCCESS) { std::cerr<<"The Vulkan loader is missing. Install a current graphics driver from your GPU manufacturer."; return 1; }
    uint32_t version=VK_API_VERSION_1_0;
    if(vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
    if(version<VK_API_VERSION_1_3) { std::cerr<<"This graphics driver does not support Vulkan 1.3. Update the graphics driver."; return 2; }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="RebrewU V3 requirements"; app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo=&app;
    VkInstance instance{};
    if(vkCreateInstance(&create,nullptr,&instance)!=VK_SUCCESS) { std::cerr<<"Vulkan 1.3 could not initialize. Check your installed graphics driver."; return 3; }
    volkLoadInstance(instance);
    uint32_t count=0;
    bool supported=false;
    if(vkEnumeratePhysicalDevices(instance,&count,nullptr)==VK_SUCCESS && count) {
        std::vector<VkPhysicalDevice> devices(count);
        if(vkEnumeratePhysicalDevices(instance,&count,devices.data())==VK_SUCCESS)
            for(auto device:devices) { VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(device,&p); supported|=p.apiVersion>=VK_API_VERSION_1_3; }
    }
    vkDestroyInstance(instance,nullptr);
    if(!supported) { std::cerr<<"No Vulkan 1.3 graphics device was found. A supported GPU and its current driver are required."; return 4; }
    std::cout<<"Windows x64 and Vulkan 1.3 requirements passed.\n";
    return 0;
}
