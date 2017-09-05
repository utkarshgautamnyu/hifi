#include <plugins/PluginManager.h>

#include <input-plugins/InputPlugin.h>

#include "daydream/DaydreamDisplayPlugin.h"
#include "daydream/DaydreamControllerManager.h"

#if defined(ANDROID)
gvr_context* __gvr_context;

DaydreamLibInstance::DaydreamLibInstance(){
    static std::once_flag once;
    std::call_once(once, [&] {
        qDebug() << __FILE__ << "has been initialized";
        
        // FIXME - this is a bit of a hack, and not quite how we expect things to be done.
        // Specifically because the application should really be responsible for loading the
        // built in plugins, and any dynamic plugins like the Daydream plugin. I needed to
        // hack in these two different versions of loadDisplayPlugins() because the Built In
        // plugins are exposed as a DisplayPluginList
        DisplayPlugin* DISPLAY_PLUGIN_POOL[] = {
            new DaydreamDisplayPlugin(),
            nullptr
        };
        // Temporarily stop using DaydreamDisplayPlugin for the Top-Down view implementation ("God View")
        //PluginManager::getInstance()->loadDisplayPlugins(DISPLAY_PLUGIN_POOL);
        DisplayPluginList builtInDisplayPlugins = getDisplayPlugins();
        PluginManager::getInstance()->loadDisplayPlugins(builtInDisplayPlugins);

		//CLIMAX_MERGE_START
		//This function was removed by the merge
        //InputPlugin* INPUT_PLUGIN_POOL[] = {
         //   new DaydreamControllerManager(),
          //  nullptr
        //};
        //PluginManager::getInstance()->loadInputPlugins(INPUT_PLUGIN_POOL);

        //InputPluginList builtInInputPlugins = getInputPlugins();
        //PluginManager::getInstance()->loadInputPlugins(builtInInputPlugins);
		//CLIMAX_MERGE_END
    });
  }

extern "C" {

JNIEXPORT void Java_io_highfidelity_hifiinterface_InterfaceActivity_nativeOnCreate(JNIEnv* env, jobject obj, jobject asset_mgr, jlong gvr_context_ptr) {
    //qDebug() << "nativeOnCreate" << gvr_context_ptr << " On thread " << QThread::currentThreadId();
    __gvr_context = reinterpret_cast<gvr_context*>(gvr_context_ptr);
}

}

 GvrState* GvrState::instance = nullptr;

 GvrState::GvrState(gvr_context *ctx) :
    _gvr_context(ctx),
    _gvr_api(gvr::GvrApi::WrapNonOwned(_gvr_context)),
    _viewport_list(_gvr_api->CreateEmptyBufferViewportList()) {

}

void GvrState::init(gvr_context *ctx)
{
    if (ctx && !instance) {
        instance = new GvrState(ctx);
    }
}

GvrState * GvrState::getInstance()
{
    return instance;            
}

#endif