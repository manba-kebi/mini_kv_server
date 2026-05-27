# Linux Sanitizer Notes

本次目标是用 ASan/UBSan 检查 mini_kv_server 在测试路径下是否存在明显内存错误和未定义行为。

![asan](C:\Users\28251\Desktop\mini_kv_server\docs\images\asan.png)

![asan_2](C:\Users\28251\Desktop\mini_kv_server\docs\images\asan_2.png)

## 使用命令

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

## 我的理解

ASan 主要检查内存访问错误，例如数组越界、use-after-free、重复释放等。

UBSan 主要检查 C/C++ 未定义行为，例如整数溢出、非法移位、空指针解引用等。

`-fno-omit-frame-pointer` 的作用是保留函数调用栈信息，方便 sanitizer 报错时看到更清楚的调用链。

## 本项目结果

本次在 ASan/UBSan 构建下运行测试，测试通过，说明当前测试覆盖到的 command 解析、KV 存储和基础网络路径没有被工具发现明显内存错误。

但这不代表代码绝对没有 bug，因为 sanitizer 只能检查“实际运行到的代码路径”。





