#include "minikv/core/key_value_store.h"
#include <cassert>
#include <string>

//测试 KV 存储模块，不启动服务端，不经过命令解析，直接验证 `set/get/erase/keys/size`
int main() {
	minikv::core::KeyValueStore store;

	assert(store.size() == 0);
	assert(!store.get("a").has_value());

	store.set("a", "1");
	assert(store.size() == 1);
	assert(store.get("a").value() == "1");

	store.set("a", "2");
	assert(store.size() == 1);
	assert(store.get("a").value() == "2");

	store.set("b", "3");
	auto keys = store.keys();
	assert(keys.size() == 2);
	assert(keys[0] == "a");
	assert(keys[1] == "b");

	assert(store.erase("a"));
	assert(!store.get("a").has_value());
	assert(!store.erase("a"));
}