# C++ Good/Bad Reference

Target: C++20. Each rule: BAD shows the mistake, GOOD shows the fix. Follow GOOD.

In this repo: RTTI is off (`-fno-rtti`), so no `dynamic_cast` or `typeid`; `std::expected` is
C++23, so use `std::optional` or a small result struct. These rules are for our code
(`libverdite/`, `port/`); patches to the decomp's game code follow the decomp's style.

---

## Resources and memory

### Own resources with RAII, never manual new/delete
```cpp
// BAD: leaks if process() throws or returns early
void f() {
    Widget* w = new Widget();
    process(w);
    delete w;
}

// GOOD
void f() {
    auto w = std::make_unique<Widget>();
    process(*w);
}
```

### Raw pointers don't own
```cpp
// BAD: who deletes this?
Widget* make_widget();

// GOOD: ownership is in the type
std::unique_ptr<Widget> make_widget();
void use(Widget* w);   // non-owning, may be null
void use(Widget& w);   // non-owning, never null
```

### Use make_unique / make_shared
```cpp
// BAD
std::unique_ptr<Foo> p(new Foo(a, b));
std::shared_ptr<Foo> s(new Foo(a, b));  // two allocations

// GOOD
auto p = std::make_unique<Foo>(a, b);
auto s = std::make_shared<Foo>(a, b);
```

### Only use shared_ptr when ownership is actually shared
```cpp
// BAD: shared_ptr by default, passed by value everywhere
void draw(std::shared_ptr<Shape> s);

// GOOD: functions that just use the object take a reference
void draw(const Shape& s);
```

### Lock with RAII
```cpp
// BAD: deadlock if anything between throws or returns
mtx.lock();
do_work();
mtx.unlock();

// GOOD
{
    std::scoped_lock lock(mtx);
    do_work();
}
```

### Never return a reference/pointer to a local
```cpp
// BAD: dangling
const std::string& name() {
    std::string s = "abc";
    return s;
}

// GOOD
std::string name() { return "abc"; }
```

### Don't keep iterators/references across container mutation
```cpp
// BAD: push_back may reallocate, first is dangling
int& first = v[0];
v.push_back(42);
use(first);

// GOOD
v.push_back(42);
use(v[0]);
```

### Erase while iterating correctly
```cpp
// BAD: it is invalid after erase
for (auto it = v.begin(); it != v.end(); ++it)
    if (bad(*it)) v.erase(it);

// GOOD
std::erase_if(v, bad);
```

---

## Lifetimes

### string_view and span don't own anything
```cpp
// BAD: the temporary string dies at the end of the line
std::string_view name = get_name();   // get_name() returns std::string
use(name);                            // dangling

// BAD: returns a view into a local
std::string_view label() {
    std::string s = build_label();
    return s;
}

// GOOD: keep the owner alive, or own the data
std::string name = get_name();
std::string label();
```

### string_view isn't null-terminated
```cpp
// BAD: C API reads past the end of the view
void open_file(std::string_view path) {
    std::fopen(path.data(), "rb");
}

// GOOD: take a string (or build one) when you need a C string
void open_file(const std::string& path) {
    std::fopen(path.c_str(), "rb");
}
```

### Don't use an object after moving from it
```cpp
// BAD: s is in a valid but unspecified state
std::string s = load();
items.push_back(std::move(s));
log(s);

// GOOD: use it before the move, or not at all
log(s);
items.push_back(std::move(s));
```

### Don't std::move a local on return
```cpp
// BAD: blocks copy elision
std::vector<int> build() {
    std::vector<int> v;
    // ...
    return std::move(v);
}

// GOOD
std::vector<int> build() {
    std::vector<int> v;
    // ...
    return v;
}
```

### Lambdas must not outlive what they capture
```cpp
// BAD: callback runs after this is destroyed
void Enemy::start() {
    timer.on_fire([this] { attack(); });
}

// BAD: captures a local by reference, stored for later
void setup() {
    int damage = 10;
    callbacks.push_back([&] { apply(damage); });
}

// GOOD: capture by value, and unregister the callback in the destructor
// (or have the timer owned by the object so it dies first)
void setup() {
    int damage = 10;
    callbacks.push_back([damage] { apply(damage); });
}
```

---

## Initialization and declarations

### Always initialize
```cpp
// BAD
int count;
Widget* w;

// GOOD
int count = 0;
Widget* w = nullptr;
```

### Use member initializers, not assignment in the constructor body
```cpp
// BAD
class Point {
    int x, y;
public:
    Point() { x = 0; y = 0; }
};

// GOOD
class Point {
    int x = 0;
    int y = 0;
};
```

### Initializer list order must match declaration order
```cpp
// BAD: b_ is initialized first, reads garbage a_
class C {
    int b_;
    int a_;
public:
    C() : a_(5), b_(a_ * 2) {}
};

// GOOD: declare in the order you depend on
class C {
    int a_;
    int b_;
public:
    C() : a_(5), b_(a_ * 2) {}
};
```

### Keep scopes small
```cpp
// BAD: i leaks out of the loop
int i;
for (i = 0; i < n; ++i) { ... }

// GOOD
for (int i = 0; i < n; ++i) { ... }

// GOOD: variable scoped to the if
if (auto it = m.find(key); it != m.end()) { ... }
```

### nullptr, not NULL or 0
```cpp
// BAD
foo(NULL);   // may call foo(int)

// GOOD
foo(nullptr);
```

### const and constexpr by default
```cpp
// BAD
int max_size = 1024;
#define BUFFER_LEN 256

// GOOD
constexpr int max_size = 1024;
constexpr std::size_t buffer_len = 256;
```

---

## Types and casts

### No C-style casts
```cpp
// BAD: silently does whatever cast compiles
auto p = (Derived*)base;
int n = (int)f;

// GOOD: say which cast you mean
auto p = static_cast<Derived*>(base);    // you know it's a Derived
// no dynamic_cast here (RTTI is off): if you need to check, the design needs a virtual or a tag
int n = static_cast<int>(f);
```

### enum class over plain enum
```cpp
// BAD: names leak into the enclosing scope, converts to int
enum Color { Red, Green };
enum Light { Red, Off };   // error: Red redeclared

// GOOD
enum class Color { Red, Green };
enum class Light { Red, Off };
```

### Mark single-arg constructors explicit
```cpp
// BAD: Buffer b = 42; compiles by accident
struct Buffer { Buffer(std::size_t n); };

// GOOD
struct Buffer { explicit Buffer(std::size_t n); };
```

### Watch signed/unsigned mixing
```cpp
// BAD: when v is empty, v.size() - 1 wraps to huge
for (int i = 0; i <= v.size() - 1; ++i)

// GOOD
for (std::size_t i = 0; i < v.size(); ++i)
// better
for (const auto& x : v)
```

### No signed overflow
```cpp
// BAD: signed overflow is UB
int x = INT_MAX;
x += 1;

// GOOD: unsigned arithmetic wraps by definition
std::uint32_t x = UINT32_MAX;
x += 1;   // 0

// GOOD: check before it happens
if (b > 0 && a > INT_MAX - b) { /* handle */ }
// or (GCC/Clang)
int sum;
if (__builtin_add_overflow(a, b, &sum)) { /* handle */ }
```

### No shifts by negative amounts or >= the bit width
```cpp
// BAD: UB if n >= 32 or n < 0
std::uint32_t mask = 1u << n;

// GOOD: mask or check the amount
std::uint32_t mask = 1u << (n & 31);
```

### No type punning through pointers
```cpp
// BAD: strict aliasing violation
float f = 1.0f;
int bits = *(int*)&f;

// GOOD
std::uint32_t bits = std::bit_cast<std::uint32_t>(f);
```

### Use fixed-width types for data with a known size
```cpp
// BAD: long is 32-bit on Windows and 64-bit on Linux
struct SaveHeader {
    long magic;
    int  flags;
};

// GOOD
struct SaveHeader {
    std::uint32_t magic;
    std::uint32_t flags;
};
static_assert(sizeof(SaveHeader) == 8);
```

### Read binary data with memcpy, not by casting the buffer
```cpp
// BAD: aliasing violation, and the buffer may not be aligned
auto* hdr = reinterpret_cast<const SaveHeader*>(buf.data());

// GOOD
SaveHeader hdr;
if (buf.size() < sizeof hdr) return std::nullopt;
std::memcpy(&hdr, buf.data(), sizeof hdr);
```

### Pin down layouts you depend on
```cpp
// GOOD: catch padding/size changes at compile time
static_assert(sizeof(Entity) == 0x40);
static_assert(offsetof(Entity, hp) == 0x10);
static_assert(std::is_trivially_copyable_v<Entity>);
```

---

## Functions and interfaces

### Pass by const reference for large read-only inputs, value for cheap ones
```cpp
// BAD
void print(std::string s);       // copies
void set(const int& n);          // pointless reference

// GOOD
void print(const std::string& s);
void print(std::string_view s);  // also accepts literals without allocating
void set(int n);
```

### Return values, not out-params
```cpp
// BAD
void parse(const std::string& in, int& out, bool& ok);

// GOOD: when "no value" is enough
std::optional<int> parse(std::string_view in);

// GOOD: when the caller needs to know why it failed (std::expected is C++23, not available)
struct ParseResult { std::optional<int> value; ParseError error; };
ParseResult parse(std::string_view in);
```

### Mark results that must not be ignored
```cpp
// BAD: caller can drop the error silently
bool save(const Game& g);

// GOOD: compiler warns if the result is discarded
[[nodiscard]] bool save(const Game& g);
```

### Pass arrays with their size
```cpp
// BAD: size is separate and easy to get wrong
void sum(int* data, int n);

// GOOD
int sum(std::span<const int> data);
```

### Keep functions short and doing one thing
```cpp
// BAD: one function that reads, parses, validates, and saves

// GOOD
auto raw    = read_file(path);
auto config = parse(raw);
validate(config);
save(config);
```

---

## Classes

### Rule of zero: let members manage themselves
```cpp
// BAD: hand-written copy/dtor for things that already manage themselves
class Name {
    char* data_;
public:
    ~Name() { delete[] data_; }
    // forgot copy ctor -> double delete
};

// GOOD
class Name {
    std::string data_;
};
```

### Rule of five: if you write one special member, write/delete all
```cpp
// GOOD
class Handle {
public:
    ~Handle();
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) noexcept;
    Handle& operator=(Handle&&) noexcept;
};
```

### Polymorphic base classes need a virtual destructor
```cpp
// BAD: deleting via Base* skips Derived's destructor
struct Base { virtual void f(); };

// GOOD
struct Base {
    virtual void f();
    virtual ~Base() = default;
};
```

### Use override
```cpp
struct Base { virtual void hit(int dmg); };

// BAD: wrong parameter type, silently declares a new function
struct D : Base { void hit(float dmg); };

// GOOD: compiler errors if it doesn't override
struct D : Base { void hit(int dmg) override; };
```

### Skip trivial getters/setters
```cpp
// BAD
class Point {
    int x_ = 0;
public:
    int get_x() const { return x_; }
    void set_x(int x) { x_ = x; }
};

// GOOD: no invariant to protect, just use a struct
struct Point {
    int x = 0;
    int y = 0;
};
```

### Mark move operations noexcept
```cpp
// BAD: vector copies instead of moving on reallocation
Buf(Buf&& other);

// GOOD
Buf(Buf&& other) noexcept;
```

---

## Error handling

### Don't ignore errors
```cpp
// BAD
std::fread(buf, 1, n, f);

// GOOD
if (std::fread(buf, 1, n, f) != n) { /* handle */ }
```

### Throw by value, catch by const reference
```cpp
// BAD
throw new std::runtime_error("x");
catch (std::exception e)   // slices

// GOOD
throw std::runtime_error("x");
catch (const std::exception& e)
```

### Don't use exceptions for normal control flow
```cpp
// BAD
int v;
try { v = m.at(key); } catch (const std::out_of_range&) { v = 0; }

// GOOD
auto it = m.find(key);
int v = (it != m.end()) ? it->second : 0;
```

---

## Standard library

### Prefer standard containers/algorithms over hand-rolled
```cpp
// BAD
int arr[100];
char buf[256]; strcpy(buf, input);   // overflow

// GOOD
std::array<int, 100> arr{};
std::vector<int> dyn;
std::string s = input;
```

### Use algorithms instead of raw loops when they say it clearly
```cpp
// BAD
bool found = false;
for (auto& x : v) if (x == target) { found = true; break; }

// GOOD
bool found = std::ranges::find(v, target) != v.end();
// C++23
bool found = std::ranges::contains(v, target);
```

### Bounds-check when the index isn't proven safe
```cpp
// BAD: unchecked index from input
int lookup(std::size_t i) {
    return table[i];
}

// GOOD
std::optional<int> lookup(std::size_t i) {
    if (i >= table.size()) return std::nullopt;
    return table[i];
}
```

### Reserve when you know the size
```cpp
// GOOD
std::vector<Item> out;
out.reserve(in.size());
for (const auto& x : in) out.push_back(transform(x));
```

---

## Concurrency

### Shared mutable data needs synchronization
```cpp
// BAD: data race
int counter = 0;
std::thread t1([&]{ ++counter; });
std::thread t2([&]{ ++counter; });

// GOOD
std::atomic<int> counter{0};
```

### Don't detach threads that touch locals
```cpp
// BAD: thread outlives the stack it references
void f() {
    int x = 0;
    std::thread([&]{ x++; }).detach();
}

// GOOD: jthread joins on scope exit
void f() {
    int x = 0;
    std::jthread t([&]{ x++; });
}
```

### Wait on condition variables with a predicate
```cpp
// BAD: spurious wakeups
cv.wait(lock);

// GOOD
cv.wait(lock, [&]{ return ready; });
```

---

## Preprocessor

### Avoid macros for constants and functions
```cpp
// BAD
#define SQUARE(x) x * x      // SQUARE(a + 1) == a + 1 * a + 1

// GOOD
constexpr auto square(auto x) { return x * x; }
```

### Use #pragma once or include guards
```cpp
// GOOD
#pragma once
```

### No `using namespace std;` in headers
```cpp
// BAD (in a .h)
using namespace std;

// GOOD: qualify names in headers
std::vector<int> v;
```

---

## Before finishing
- No raw new/delete, no owning raw pointers
- Everything initialized, including struct members
- No C-style casts, no NULL
- No views (string_view, span) or lambda captures outliving their data
- No use after move
- No signed overflow, bad shifts, or pointer type punning
- Fixed-width types and static_asserts on any struct with a known layout
- Polymorphic bases have virtual destructors; overrides marked override
- No ignored return values on fallible calls; [[nodiscard]] on your own
- No shared mutable state without a lock or atomic
- Compiles clean with -Wall -Wextra and passes clang-tidy
- Debug builds run clean under -fsanitize=address,undefined
