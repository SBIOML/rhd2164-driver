cd tests
cmake -S. -Bbuild && cd build
cmake --build . && ctest --output-on-failure
cd ../..