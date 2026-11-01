#!/usr/bin/env python3
"""Fail when a source uses a standard-library name without including the header that declares it.

    python3 tests/std_headers_check.py <source dir>

Why: libstdc++ (the Linux PR build) pulls many standard headers in through others, so code that calls std::max with
only <cmath> included compiles on Linux and then stops MSVC on main (Perspective.h, found after PR #53).  The
portability check (portability_check.py) can't see this because it still compiles against libstdc++.

How: for each .cpp/.h/.hpp/.mm under src/ and tests/, strip // comments, look for the std names in HEADERS below, and
accept a name when its header is included by the file itself or by any project header it includes, followed
recursively (a project header is part of the file's own contract, so that is deterministic on every compiler; a
standard header including another standard header is not).  It is a text scan, so it also covers #ifdef _WIN32 and
__APPLE__ code that Linux never compiles.  The table lists names whose header is commonly forgotten, not the whole
library; <utility> is left out because every container header provides std::pair and std::move on all three STLs.

Exits 1 listing "file: std::name needs <header>" for each miss.  Runs in well under a second.
"""
import os
import re
import sys

HEADERS = {
    'algorithm': r'std::(max|min|minmax|sort|stable_sort|clamp|find|find_if|count|count_if|any_of|all_of|none_of|'
                 r'remove|remove_if|reverse|fill|copy_if|unique|lower_bound|upper_bound|max_element|min_element|'
                 r'nth_element|transform|equal|partial_sort|binary_search|for_each|rotate|minmax_element|partition)\b',
    'numeric': r'std::(accumulate|iota|inner_product|partial_sum|gcd|lcm)\b',
    'functional': r'std::(function|bind|hash|greater|less)\b',
    'memory': r'std::(unique_ptr|shared_ptr|make_unique|make_shared|weak_ptr)\b',
    'optional': r'std::(optional|nullopt)\b',
    'array': r'std::array\b',
    'map': r'std::map\b',
    'set': r'std::set\b',
    'unordered_map': r'std::unordered_map\b',
    'unordered_set': r'std::unordered_set\b',
    'deque': r'std::deque\b',
    'tuple': r'std::(tuple|tie)\b',
    'thread': r'std::(thread|this_thread)\b',
    'mutex': r'std::(mutex|lock_guard|unique_lock|scoped_lock)\b',
    'condition_variable': r'std::condition_variable\b',
    'atomic': r'std::atomic\b',
    'chrono': r'std::chrono\b',
    'sstream': r'std::(ostringstream|istringstream|stringstream)\b',
    'fstream': r'std::(ifstream|ofstream|fstream)\b',
    'limits': r'std::numeric_limits\b',
    'stdexcept': r'std::(runtime_error|invalid_argument|out_of_range|logic_error)\b',
    'iomanip': r'std::(setprecision|setw|put_time|get_time)\b',
    'regex': r'std::regex\b',
    'filesystem': r'std::filesystem\b',
    'random': r'std::(mt19937|random_device|uniform_\w+_distribution)\b',
    'cmath': r'std::(sqrt|fabs|floor|ceil|round|lround|pow|exp|log|atan2|hypot|isnan|isfinite|sin|cos|tan|fmod)\b',
    'cstring': r'\b(std::)?(memcpy|memset|strcmp|strlen|strncpy|memcmp|strchr|strstr)\s*\(',
    'vector': r'std::vector\b',
    'string': r'std::(string|to_string|stoi|stod|stof|getline)\b',
}
# C spellings that satisfy the same need.
ALSO = {'cmath': {'math.h'}, 'cstring': {'string.h'}}
INCLUDE = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
SOURCE = ('.cpp', '.h', '.hpp', '.mm')


def main(src_root):
    files = [os.path.normpath(os.path.join(d, f)) for top in ('src', 'tests')
             for d, _, names in os.walk(os.path.join(src_root, top)) for f in names if f.endswith(SOURCE)]
    by_name = {}
    for f in files:
        by_name.setdefault(os.path.basename(f), f)

    def included(path, seen):
        """Every #include name reachable from path through project headers (resolved beside the file, then by name,
        the way VST's include directories make bare names like "Archive.h" work from tests/)."""
        if path in seen:
            return set()
        seen.add(path)
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            return set()
        out = set()
        for name in INCLUDE.findall(text):
            out.add(os.path.basename(name))
            beside = os.path.normpath(os.path.join(os.path.dirname(path), name))
            target = beside if os.path.exists(beside) else by_name.get(os.path.basename(name))
            if target:
                out |= included(target, seen)
        return out

    misses = []
    for f in sorted(files):
        code = re.sub(r'//.*', '', open(f, encoding='utf-8', errors='replace').read())
        have = included(f, set())
        for header, pattern in HEADERS.items():
            m = re.search(pattern, code)
            if m and not ({header} | ALSO.get(header, set())) & have:
                misses.append(f'{os.path.relpath(f, src_root)}: {m.group(0).rstrip("( ")} needs <{header}>')
    for line in misses:
        print(line)
    if misses:
        print(f'{len(misses)} standard header(s) missing: MSVC or libc++ will not find these names')
    return 1 if misses else 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
