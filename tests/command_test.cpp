#include <cassert>
#include <string>
#include "minikv/core/command.h"

//测试命令解析模块，不需要启动网络服务。它直接调用 `parse_command()`，验证输入字符串能否变成正确的 `Command`。
int main() {
	using minikv::core::CommandType;
	using minikv::core::parse_command;

	auto ping = parse_command("PING");
	assert(ping.type == CommandType::Ping);

	auto set = parse_command("set name zhao");
	assert(set.type == CommandType::Set);
	assert(set.key == "name");
	assert(set.value == "zhao");

	auto get = parse_command("GET name");
	assert(get.type == CommandType::Get);
	assert(get.key == "name");

	auto bad = parse_command("GET a b");
	assert(bad.type == CommandType::Invalid);
	assert(!bad.error.empty());
}