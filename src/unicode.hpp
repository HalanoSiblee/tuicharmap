#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uc {

std::string name(uint32_t codepoint);
bool is_printable(uint32_t codepoint);

void warmup();
bool ready();
size_t count();

uint32_t       cp_at(size_t i);
uint8_t        width_at(size_t i);
std::string_view upper_name(size_t i);

size_t find(uint32_t codepoint);

} // namespace uc