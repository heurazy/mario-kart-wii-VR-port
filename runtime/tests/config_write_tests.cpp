#include "runtime_config.h"
#include <chrono>
#include <iostream>
int main() {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(root);
    const auto path=root/"Config.toml";
    const auto read=[&] { std::ifstream f(path);return std::string(std::istreambuf_iterator<char>(f),{}); };
    mkw::platform::AtomicWriteText(path,"# Keep my settings\n[vr]\nenabled = true\n\n[video]\nwindow_width = 854\n");
    bool ok=RuntimeConfigFile::WriteSettingAtPath(path,"vr","default_camera","1");
    RuntimeConfigFile::Mutable().wiiContinuousScan.reset();
    ok &= !RuntimeConfigFile::WiiContinuousScanEnabled();
    RuntimeConfigFile::Mutable().wiiContinuousScan = true;
    ok &= RuntimeConfigFile::WiiContinuousScanEnabled();
    RuntimeConfigFile::Mutable().wiiContinuousScan = false;
    ok &= !RuntimeConfigFile::WiiContinuousScanEnabled();
    std::istringstream legacyScan("[controller]\nwii_continuous_scan = true\n");
    ok &= !RuntimeConfigFile::ParseConfig(legacyScan,"legacy").wiiContinuousScan.value_or(false);
    std::istringstream explicitScan("[controller]\nwii_continuous_scan_opt_in = true\n");
    ok &= RuntimeConfigFile::ParseConfig(explicitScan,"opt-in").wiiContinuousScan.value_or(false);
    ok &= RuntimeConfigFile::WriteSettingAtPath(path,"vr","welcome_complete","true");
    ok &= RuntimeConfigFile::WriteSettingAtPath(path,"vr","tutorial_completed","3");
    ok &= RuntimeConfigFile::WriteSettingAtPath(path,"vr","default_camera","2");
    ok &= RuntimeConfigFile::WriteSettingAtPath(path,"controller","wii_continuous_scan_opt_in","false");
    const auto content=read();
    std::istringstream stream(content);
    const auto config=RuntimeConfigFile::ParseConfig(stream,"test");
    ok &= config.vrWelcomeComplete && config.vrDefaultCamera==2 && config.vrTutorialCompleted==3;
    ok &= content.find("# Keep my settings")!=std::string::npos && content.find("window_width = 854")!=std::string::npos;
    fs::create_directory(root/"Config.toml.pending");
    ok &= !RuntimeConfigFile::WriteSettingAtPath(path,"vr","default_camera","0") && read()==content;
    fs::remove(root/"Config.toml.pending");
    ok &= RuntimeConfigFile::WriteSettingAtPath(root/"new.toml","vr","welcome_complete","true");
    fs::remove_all(root);
    if(!ok) std::cerr<<"Configuration save regression failed\n";
    return ok?0:1;
}
