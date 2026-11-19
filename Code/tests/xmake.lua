
target("TPTests")
    set_kind("binary")
    set_group("Tests")
    add_includedirs(
        ".", "../encoding")
    add_headerfiles("**.h")
    add_files("*.cpp")
    -- Has no game dependencies, so the client's remote playback timing can be tested here
    add_files("../client/Systems/PlaybackClock.cpp")
    add_files("../server/Campaign/**.cpp")
    add_deps("SkyrimEncoding")
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "catch2",
        "cryptopp",
        "zlib",
        "mimalloc",
        "glm")

if is_plat("linux") then
    target("CampaignSmokeClient")
        set_kind("binary")
        set_group("Tests")
        set_default(false)
        add_defines("STEAMNETWORKINGSOCKETS_STATIC_LINK")
        add_files("integration/CampaignClient.cpp")
        add_includedirs("../encoding")
        add_deps("SkyrimEncoding", "TiltedConnect")
        add_packages("tiltedcore", "hopscotch-map", "gamenetworkingsockets", "glm")
end
