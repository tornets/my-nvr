conan install . -s build_type=Release --output-folder=build --build=missing
conan build . -s build_type=Release --output-folder=build --build=missing


conan build . -s build_type=Debug --output-folder=build --build=missing
