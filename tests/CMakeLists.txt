add_executable(test_terrain test_terrain.cpp)
target_link_libraries(test_terrain PRIVATE jke_sim)

add_executable(test_economy test_economy.cpp)
target_link_libraries(test_economy PRIVATE jke_sim)

add_executable(test_battle test_battle.cpp)
target_link_libraries(test_battle PRIVATE jke_sim)

add_test(NAME TerrainTests COMMAND test_terrain)
add_test(NAME EconomyTests COMMAND test_economy)
add_test(NAME BattleTests  COMMAND test_battle)
