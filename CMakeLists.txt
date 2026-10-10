cmake_minimum_required(VERSION 3.21)
project(PureRemote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Svg Network)

add_executable(PureRemote WIN32
    src/main.cpp
    src/DarkTheme.cpp
    src/I18n.cpp
    src/AppConfig.cpp
    src/Autostart.cpp
    src/RustDeskController.cpp
    src/PasswordPolicy.cpp
    src/ConnectionWatcher.cpp
    src/NotifyPolicy.cpp
    src/TsnetBridge.cpp
    src/TrayController.cpp
    src/AddressBook.cpp
    src/SelfCheck.cpp
    src/ConnectPanel.cpp
    src/ConnectSession.cpp
    src/NetworkProbe.cpp
    src/NetworkProfile.cpp
    src/Guide.cpp
    src/PeerPrefs.cpp
    src/MainWindow.cpp
    resources/app.qrc
)

target_include_directories(PureRemote PRIVATE src)

target_link_libraries(PureRemote PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::Svg
    Qt6::Network          # 单例复用：QLocalServer/QLocalSocket 叫醒热实例
)

if(WIN32)
    # shell32 打开资源管理器定位文件；dwmapi 深色标题栏
    target_link_libraries(PureRemote PRIVATE shell32 dwmapi)
    # 图标 + 版本资源（windres 编译 app.rc，-I 供其定位 icon.ico）
    enable_language(RC)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_SOURCE_DIR}/resources")
    target_sources(PureRemote PRIVATE resources/app.rc)
endif()

target_compile_options(PureRemote PRIVATE -O2 -DNDEBUG)
