---
description: Build the MyNVR project using conan
allowed-tools: Bash(conan build *)
---

Execute the following build command for the MyNVR project:

```bash
conan build . -of=build -s build_type=Debug -s compiler.cppstd=17 --build=missing
```

After the build completes, report whether it succeeded or failed. If it failed, show the relevant error output.