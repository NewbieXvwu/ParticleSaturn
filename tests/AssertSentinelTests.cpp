// 测试基础设施哨兵（D-008，AUDIT P0-2）：守住两条曾经整体失效的底线——
// 1. 断言必须在测试构建中生效（曾被 Release 的 NDEBUG 整体剥离，34/34 空转）；
// 2. 测试失败必须以非零退出码传播到 ctest（曾被 [NSApp terminate:] 吞成 0）。
//
// 机制：断言生效时表达式副作用会执行，本程序以 return 1 退出，注册时的
// WILL_FAIL 把它翻转为"通过"；一旦断言再度被剥离，程序退出 0，WILL_FAIL
// 将其翻转为"失败"报警。故意不用 assert(false)：ctest 把信号型退出记为
// Exception，WILL_FAIL 对其不生效。
#include <cassert>
#include <cstdio>

int main() {
    bool assertsLive = false;
    assert((assertsLive = true));
    if (!assertsLive) {
        std::fprintf(stderr, "[sentinel] NDEBUG stripped assertions from a test target; "
                             "the -UNDEBUG sweep in the top-level CMakeLists is broken\n");
        return 0;
    }
    return 1;
}
