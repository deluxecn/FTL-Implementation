
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <cxxabi.h>
#include <sys/mman.h>
#include <unistd.h>
#include <iostream>

#include <map>
#include <string>
#include <vector>

#define MAP_FILE_NAME ("746FlashSim.map")
#define SEC_FILE_NAME ("746FlashSim.sec")
#define LIB_MAP_FILE_NAME ("libs.map")

#define READELF_LINE_SIZE 4096
#define READELF_TOKEN_SIZE 4096

#define MEM_USAGE_FILE ("mem_size_list.txt")
#define MEM_LEAK_FILE ("mem_leak_list.txt")
#define ALLOC_DIST_FILE ("mem_dist_list.txt")
#define STACK_SIZE_FILE ("stack_size_list.txt")

#define INS_JMP ('\xE9')
#define INS_CALL ('\xE8')


using namespace std;

/*
 * Function Prototypes
 */
typedef void *(*malloc_func_t)(size_t);
typedef void (*free_func_t)(void *);
typedef void *(*realloc_func_t)(void *, size_t);

/*
 * Real function pointers
 */
static malloc_func_t real_malloc = NULL;
static free_func_t real_free = NULL;
static realloc_func_t real_realloc = NULL;

/*
 * Flags to avoid recursion
 */
static bool before_init = true;
static bool reentry = false;
static bool stack_growing = false;

/*
 * Counter variable
 */
static int malloc_count = 0;
static int free_count = 0;
static int realloc_count = 0;
static unsigned long stack_change_count = 0;

// This is increased everytime there is a malloc/free
static unsigned long global_counter = 0;

/*
 * Size statistics variable
 */
// This accounts for free()
static size_t max_allocated = 0;
static size_t current_allocated = 0;
// This does not count free()
static size_t total_allocated = 0;
static size_t total_freed = 0;

/* The maximum number of mallc() call at a certain time */
static size_t max_malloc_count = 0;
static size_t current_malloc_count = 0;
/*
 * Function pointers
 */
static unsigned char *main_ptr = NULL;
static size_t main_size = 0;

static unsigned char *init_ptr = NULL;
static size_t init_size = 0;

static unsigned char *libc_start_main_ptr = NULL;
static unsigned char restore_array[5];

/*
 * Stack usage variable
 */
// This is the stack pointer when this is library loaded
// We treat this as the bottom of the stack which is not 100%
// accurate but with acceptable accuracy
static unsigned char *initial_stack = NULL;
static unsigned char *recent_stack = NULL;
static unsigned long max_stack_size = 0;

/*
 * struct MemObject - Symbol table entry
 */
struct MemObject
{
    unsigned char *ptr;
    size_t obj_len;
    // For library function this is raw name
    // for application function this is demangled name
    string name;

    // For library function this is always true
    // For application function this might be false
    // if the target is dynamically linked
    bool is_static;
};

// Symbol table starting address and size
static map<void *, MemObject> *object_map_p;
// Use library function name to find its size
static map<string, MemObject> *lib_map_p;

// The frequency of allocation
static map<size_t, int> *alloc_count_p;

struct MemAlloc
{
    void *base;
    size_t bytes;
    string name;
    unsigned long counter;
};

// Holds memory objects
static map<void *, MemAlloc> *mem_map_p;
static vector<size_t> *mem_size_list_p;

static vector<pair<unsigned long, unsigned long> > *stack_size_list_p;

struct SectionInfo
{
    string name;
    unsigned char *mem_ptr;
    unsigned long file_offset;
    size_t bytes;
};

static map<string, SectionInfo> *section_map_p;

string *GetReturnName(void *ptr)
{
    unsigned char *b = NULL;
    unsigned char *e = NULL;

    for(map<void *, MemObject>::iterator it = object_map_p->begin();
        it != object_map_p->end();
        it++)
    {
        b = it->second.ptr;
        e = b + it->second.obj_len;

        if((ptr >= (void *)b) && (ptr < (void *)e))
        {
            return &(it->second.name);
        }
    }

    return NULL;
}

bool IsEndOfStack(void *return_ptr, void **stack_frame)
{
    if(main_ptr && \
       (return_ptr >= (void *)main_ptr) && \
       (return_ptr < (void *)(main_ptr + main_size)))
    {
        return true;
    }

    if(init_ptr && \
       (return_ptr >= (void *)init_ptr) && \
       (return_ptr < (void *)(init_ptr + init_size)))
    {
        return true;
    }

    if(((unsigned long)stack_frame & 0x700000000000) == 0x00UL)
    {
        return true;
    }

    // We do not allow it to cross the stack bottom when this library
    // is initialized (that's near the bottom of the stack)
    if((unsigned long)stack_frame > (unsigned long)initial_stack)
    {
        return true;
    }

    return false;
}

string GetFirstRecognizableName()
{
    void **stack_frame = (void **)__builtin_frame_address(1);
    void *return_ptr = NULL;
    string *return_name = NULL;

    do
    {
        return_ptr = *(stack_frame + 1);
        // This is stored in object map, do not free this
        return_name = GetReturnName(return_ptr);

        if(return_name != NULL)
        {
            return *return_name;
        }

        stack_frame = (void **)*stack_frame;
    } while(!IsEndOfStack(return_ptr, stack_frame));

    return string("[Unknown name]");
}

/*
 * PrintStackTrace() - Return heap memory that holds all stack pointers
 *
 * Return value is the number of stack pointers actual captured
 * NOTE: Need to free the memory after using it
 */
void PrintStackTrace()
{
    // We don't use 0 since it is in the address of our function
    // If this function is called in another deeper place
    // then probably we need to make it smarter
    void **stack_frame = (void **)__builtin_frame_address(1);
    void *return_ptr = NULL;
    string empty_name = string("[No Name]");

    do
    {
        return_ptr = *(stack_frame + 1);
        string *return_name = GetReturnName(return_ptr);
        if(return_name == NULL)
        {
            return_name = &empty_name;
        }

        printf("stack frame = %p, return = %p\n\tname = %s\n",
               stack_frame,
               return_ptr,
               return_name->c_str());

        stack_frame = (void **)*stack_frame;
    } while(!IsEndOfStack(return_ptr, stack_frame));

    printf("*******************************\n");

    return;
}

unsigned char simple_alloc[4096];
unsigned char *simple_alloc_p = simple_alloc;

void *realloc(void *p, size_t byte)
{
    if(!before_init)
    {
        // We only count from outside the library
        if(!reentry)
        {
            realloc_count++;
        }

        return real_realloc(p, byte);
    }

    return NULL;
}

void free(void *p)
{
    if(p == NULL)
    {
        return;
    }

    if(!before_init)
    {
        // We only count free from outside
        if(!reentry)
        {
            reentry = true;

            free_count++;
            global_counter++;

            map<void *, MemAlloc>::iterator it = mem_map_p->find(p);
            if(it == mem_map_p->end())
            {
                fprintf(stderr, "ERROR: free() illegal pointer %p\n", p);
                exit(1);
            }

            // Decrease current memory usage
            current_allocated -= it->second.bytes;
            // std::cout << "current_allocated is " << current_allocated << std::endl;
            total_freed += it->second.bytes;

            current_malloc_count--;

            // Record current memory usage
            // mem_size_list_p->push_back(current_allocated);

            mem_map_p->erase(p);

            reentry = false;
        }

        real_free(p);
    }

    return;
}

/*
 * malloc() - Mock library that intercepts malloc() call by
 * library and user application
 */
void *malloc(size_t bytes)
{
    // Use the simple allocator before everything has been
    // set up since dlsym() itself calls malloc
    if(before_init)
    {
        void *temp = (void *)simple_alloc_p;
        simple_alloc_p += bytes;

        // Considering alignment, we need not to
        // free space, so just mesaure the distance between
        // the two pointers
        if(simple_alloc_p - simple_alloc >= 4088)
        {
            assert(false);
        }

        return temp;
    }

    // Only trace stack when there is no recursion
    if(!reentry)
    {
        reentry = true;

        // Only count heap allocation from outside the library
        malloc_count++;

        if(alloc_count_p->find(bytes) == alloc_count_p->end())
        {
            (*alloc_count_p)[bytes] = 1;
        }
        else
        {
            (*alloc_count_p)[bytes]++;
        }

        //PrintStackTrace();

        reentry = false;
    }

    void *base = real_malloc(bytes);

    if(!reentry)
    {
        reentry = true;

        // We could not allocate the same piece of memory twice
        assert(mem_map_p->find(base) == mem_map_p->end());

        MemAlloc mem_alloc;
        mem_alloc.base = base;
        mem_alloc.bytes = bytes;
        mem_alloc.name = GetFirstRecognizableName();
        mem_alloc.counter = global_counter;

        // Update variables
        total_allocated += bytes;
        current_allocated += bytes;
        current_malloc_count++;
        if(current_malloc_count > max_malloc_count) {
          max_malloc_count = current_malloc_count;
        }

        // std::cout << "current_allocated is " << current_allocated << std::endl;
        global_counter++;

        // Update data structures
        // mem_size_list_p->push_back(current_allocated);

        if(max_allocated < current_allocated)
        {
            max_allocated = current_allocated;
        }

        (*mem_map_p)[base] = mem_alloc;

        reentry = false;
    }

    return base;
}

/*
 * GetNextToken() - Get next token from the file delimited by space
 *
 * If get_all is true then this function will fill the buffer with
 * whatever left in the current line unread, which might be padded with
 * space. Next read will just start a new line.
 */
static bool GetNextToken(FILE *fp, char *token_p, bool get_all=false)
{
    static char buffer[READELF_LINE_SIZE] = {};
    static bool need_read = true;
    static int offset = 0;

    bool is_line_head = false;

    while(1)
    {
        if(need_read == true)
        {
            // fgets will copy the newline character if there is one
            void *ret = fgets(buffer, READELF_LINE_SIZE, fp);
            if(ret == NULL)
            {
                token_p[0] = EOF;
                token_p[1] = '\0';

                return false;
            }

            // If we read empty line then do not bother processing
            // just jump to next line
            // Need to consider both last line ('\0') and
            // middle lines ('\n')
            if(buffer[0] == '\n' || buffer[0] == '\0')
            {
                continue;
            }

            is_line_head = true;
            need_read = false;
            offset = 0;

            //printf("New line: %s", buffer);
        }

        if(get_all == true)
        {
            strcpy(token_p, buffer + offset);
            need_read = true;
            break;
        }

        int scan_ret = sscanf(buffer + offset, "%s", token_p);

        if(scan_ret == EOF)
        {
            need_read = true;
            continue;
        }

        char *p = strstr(buffer + offset, token_p);
        //printf("Token = %s\n", token_p);
        assert(p);
        offset = (p - buffer) + strlen(token_p);

        break;
    }

    return is_line_head;
}

/*
 * ParseSectionLayout() - Parse the section layout produced by readdlf
 *
 * We only keep section name, memory address, file offset, and size, and
 * maintain these information using a map indexed by name
 */
void ParseSectionLayout(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if(fp == NULL)
    {
        fprintf(stderr, "Cannot open section layout file: %s\n", filename);
        exit(1);
    }

    char token[READELF_TOKEN_SIZE];
    while(1)
    {
        bool is_line_head = GetNextToken(fp, token);
        char first_ch = token[0];

        if(first_ch == EOF)
        {
            break;
        }

        int token_len = strlen(token);
        char last_ch = token[token_len - 1];

        // The first token in a line with ':' as the last character
        if(is_line_head && last_ch == ']' && first_ch == '[')
        {
            if((token_len < 3) || !isdigit(token[token_len - 1 - 1]))
            {
                // God bless me!
                goto finish_this_line;
            }

            // Rip off [ and ] and update token length to reflect the truth
            token[token_len - 1] = '\0';
            token_len--;

            // We use token + 1 to ignore the starting '['
            int index = atoi(token + 1);
            if(index == 0)
            {
                // The first entry is always empty
                goto finish_this_line;
            }

            GetNextToken(fp, token);
            if(token[0] != '.')
            {
                goto finish_this_line;
            }

            string sec_name(token);

            GetNextToken(fp, token);
            GetNextToken(fp, token);
            unsigned char *address = \
                (unsigned char *)strtoul(token, NULL, 16);

            GetNextToken(fp, token);
            unsigned long file_address = strtoul(token, NULL, 16);

            GetNextToken(fp, token);
            size_t sz = strtoul(token, NULL, 16);

            // Construct the object and put it into the map
            SectionInfo section;
            section.bytes = sz;
            section.file_offset = file_address;
            section.mem_ptr = address;
            section.name = sec_name;

            if(section_map_p->find(sec_name) != section_map_p->end())
            {
                fprintf(stderr,
                        "WARNING: Section %s has already been registered\n"
                        "         Overwrite existing section information\n",
                        sec_name.c_str());
            }

            (*section_map_p)[sec_name] = section;
        }

finish_this_line:
        GetNextToken(fp, token, true);
    }

    return;
}

void ParseBinaryLayoutCallback(char *token, unsigned char *object_ptr, size_t object_size)
{
    // Create an object and copy assign it into
    // the ordered map
    MemObject mem_obj;

    // 1. Re-assign length and pointer if pointer is NULL
    // 2. Assign a demangled name
    mem_obj.obj_len = object_size;
    mem_obj.ptr = (unsigned char *)object_ptr;

    // This might be changed to false
    mem_obj.is_static = true;

    int status;
    const char *demangle_name;

    // It implies this is a dynamically linked object
    if(object_ptr == NULL)
    {
        mem_obj.is_static = false;
        // First we search in library catalog to see whether there is an entey
        // NOTE: We use the long name even with "@"
        map<string, MemObject>::iterator it = lib_map_p->find(token);
        if(it != lib_map_p->end())
        {
            mem_obj.obj_len = it->second.obj_len;

            //fprintf(stderr,
            //        "Resolved dynamic object size for (raw name) \"%s\"\n"
            //        "    size = %lu\n",
            //        token,
            //        mem_obj.obj_len);
        }
        else
        {
            // fprintf(stderr,
            //         "Still could not resolve length of dynamic symbol (raw name) \"%s\"\n",
            //         token);

            return;
        }

        // Chop off the substring after "@" that denotes library version
        // Not sure whether this is portable - but if there is no "@" in
        // the name then we just use the raw name
        // NOTE: Either dlsym() and demangle would require there is no such
        // "@" stuff
        char *p = strstr(token, "@");
        if(p != NULL)
        {
            *p = '\0';
        }

        // We still use token here
        object_ptr = (unsigned char *)dlsym(RTLD_NEXT, token);
        if(object_ptr == NULL)
        {
            // fprintf(stderr,
            //         "Ignoring (raw) name \"%s\" "
            //         "- No object found by dlsym()\n",
            //         token);

            return;
        }
        else
        {
            mem_obj.ptr = object_ptr;
            //fprintf(stderr, "    object location = %p\n", object_ptr);
        }
    }
    else
    {
        //fprintf(stderr, "Resolved static symbol %s\n", token);
    }

    // Demangle the name here, and if it fails we just
    // fall back to what has been read from the map file
    demangle_name = abi::__cxa_demangle(token, NULL, NULL, &status);
    if(status != 0)
    {
        // This is important since for pure C library functions their
        // name are not mangled, so they are for sure to fail demangling
        // and we need to take care of this situation carefully
        mem_obj.name = string(token);
    }
    else
    {
        mem_obj.name = string(demangle_name);
        free((void *)demangle_name);
    }

    //fprintf(stderr,
    //        "    Object name = %.40s %s\n",
    //        mem_obj.name.c_str(),
    //        mem_obj.name.size() > 40 ? "(...)" : "");

    (*object_map_p)[object_ptr] = mem_obj;

    if(strcmp(token, "main") == 0)
    {
        main_ptr = (unsigned char *)object_ptr;
        main_size = object_size;

        /* fprintf(stderr,
                "Found main function [%p, %p)\n",
                main_ptr,
                main_ptr + object_size); */
    }
    else if(strcmp(token, "__libc_csu_init") == 0)
    {
        init_ptr = (unsigned char *)object_ptr;
        init_size = object_size;

        /* fprintf(stderr,
                "Found init function [%p, %p)\n",
                main_ptr,
                main_ptr + object_size); */
    }

    return;
}

/*
 * ParseGlibLayoutCallback() - Parse binary layout for shared library
 *
 * NOTE: For shared libraries, we only store their raw name even with "@"
 * in the name field. This is used to simplify searching using the raw
 * name read from application's dynamic symbol table.
 *
 * However for application's own symbol (i.e. in object_map_p) we always store
 * demangled name after chopping off the "@" part
 */
void ParseGlibLayoutCallback(char *token, unsigned char *object_ptr, size_t object_size)
{
    MemObject mem_obj;
    mem_obj.obj_len = object_size;
    mem_obj.ptr = object_ptr;
    mem_obj.is_static = true;

    // If the size is 0 then it makes no sense saving it
    // It usually implies that object ptr is NULL
    if(object_size == 0LU)
    {
        return;
    }

    (*lib_map_p)[token] = mem_obj;

    return;
}

/*
 * ParseBinaryLayout() - Parse the file generated by readelf utility
 *
 * We ignore every symbol that has 0x0000 base address (dynamically linked)
 * and those that are not of FUNC type
 */
void ParseBinaryLayout(const char *filename, void (*callback)(char *, unsigned char *, size_t))
{
    FILE *fp = fopen(filename, "r");
    if(fp == NULL)
    {
        fprintf(stderr, "Cannot open map file: %s\n", filename);
        exit(1);
    }

    char token[READELF_TOKEN_SIZE];
    while(1)
    {
        bool is_line_head = GetNextToken(fp, token);
        char first_ch = token[0];

        if(first_ch == EOF)
        {
            break;
        }

        char last_ch = token[strlen(token) - 1];

        // The first token in a line with ':' as the last character
        if(is_line_head && last_ch == ':')
        {
            GetNextToken(fp, token);
            unsigned char *object_ptr = (unsigned char *)strtol(token, NULL, 16);

            GetNextToken(fp, token);
            size_t object_size = (size_t)strtol(token, NULL, 10);

            // Only use FUNC type
            GetNextToken(fp, token);
            if(strcmp(token, "FUNC") != 0)
            {
                // Dijkstra will bite me
                goto finish_this_line;
            }

            // We do not use the following 3 tokens
            GetNextToken(fp, token);
            GetNextToken(fp, token);
            GetNextToken(fp, token);

            GetNextToken(fp, token);

            // Let the callback do actual stuff
            callback(token, object_ptr, object_size);
        }

        // Finish the current line
finish_this_line:
        GetNextToken(fp, token, true);
    }

    fclose(fp);
    return;
}

/*
 * ProfileStack() - Stack profiler which will be called everytime
 * a function exits
 *
 * We inject JMP instruction into gnu library's function hook body
 * in order to get to this function
 */
void __attribute__((optimize("O0"))) ProfileStack(void *func, void *caller)
{
    reentry = true;
    register unsigned long rsp asm ("rsp");

    (void)func;
    (void)caller;

    stack_change_count++;
    if(recent_stack == NULL)
    {
        goto record_recent_stack;
    }
    else if((unsigned char *)rsp > recent_stack)
    {
        if(stack_growing == false)
        {
            stack_growing = true;
            //stack_size_list_p->push_back(make_pair(stack_change_count,
            //                             (unsigned long)initial_stack - rsp));
        }
    }
    else if((unsigned char *)rsp <= recent_stack)
    {
        if(stack_growing == true)
        {
            stack_growing = false;
            //stack_size_list_p->push_back(make_pair(stack_change_count,
            //                             (unsigned long)initial_stack - rsp));
        }
    }

record_recent_stack:
    recent_stack = (unsigned char *)rsp;
    unsigned long current_stack_size = (unsigned long)(initial_stack - recent_stack);
    if(current_stack_size > max_stack_size)
    {
        max_stack_size = current_stack_size;
    }

    reentry = false;
    return;
}

/*
 * InjectFunction() - Inject a function with JMP instruction to a target
 *
 * We could optionally store the overwritten bytes in original_bytes variable
 * and use this to restore default behavior
 */
void InjectFunction(unsigned char ins,
                    unsigned char *from_ptr,
                    unsigned char *to_ptr,
                    unsigned char *original_bytes)
{
    // Compute distance between the two pointers
    // We minus 5 because x86-64 calculates offsets using the next instruction
    // offset address
    unsigned long ptr_diff = to_ptr - from_ptr - 5;
    unsigned char *ptr_diff_ptr = (unsigned char *)&ptr_diff;

    // Disable memory protection
    int page_size = getpagesize();
    int ret = mprotect((void *)((unsigned long)from_ptr & ~((unsigned long)page_size - 1)),
                       page_size,
                       PROT_READ|PROT_WRITE|PROT_EXEC);
    assert(ret == 0);

    if(original_bytes != NULL)
    {
        original_bytes[0] = from_ptr[0];
        original_bytes[1] = from_ptr[1];
        original_bytes[2] = from_ptr[2];
        original_bytes[3] = from_ptr[3];
        original_bytes[4] = from_ptr[4];
    }

    // Inject JMP/CALL instruction to the function body
    from_ptr[0] = ins;
    from_ptr[1] = ptr_diff_ptr[0];
    from_ptr[2] = ptr_diff_ptr[1];
    from_ptr[3] = ptr_diff_ptr[2];
    from_ptr[4] = ptr_diff_ptr[3];

    return;
}

/*
 * RestoreFunction() - Restore an injected function
 */
void RestoreFunction(unsigned char *original_bytes,unsigned char *ptr)
{
    for(int i = 0;i < 5;i++)
    {
        *(((unsigned char *)ptr) + i) = \
            *(((unsigned char *)original_bytes) + i);
    }

    return;
}

/*
 * InjectStackProfiler() - Inject a JMP Instruction to the head of
 * cygwin profiler so that on every function exit it will jump to
 * our customized stack profiler
 */
void InjectStackProfiler()
{
    // fprintf(stderr, "&__cyg_profile_func_exit = %p\n", dlsym(RTLD_NEXT, "__cyg_profile_func_exit"));
    // fprintf(stderr, "&ProfileStack = %p\n", &ProfileStack);

    unsigned char *cyg_profile_ptr = (unsigned char *)dlsym(RTLD_NEXT, "__cyg_profile_func_exit");
    unsigned char *custom_profile_ptr = (unsigned char *)&ProfileStack;

    // Inject the function and not bothering restoring the content
    // since we always need this interrupt
    InjectFunction(INS_JMP, cyg_profile_ptr, custom_profile_ptr, NULL);

    return;
}

/*
int m(int argc, char **argv, char **envp)
{
    fprintf(stderr, "main(), I am your father\n");
    return 0;
}
*/

/*
 * LibcStartMain() - __libc_start_main()'s hijacker
 *
 * NOTE: Since it is compiled in the library, it uses stack frame
 * so that we must use rpb instead of rsp
 */
void __attribute__((optimize("O0"))) LibcStartMain()
{
    reentry = true;

    // These are not real variables; they just alias
    // the registers and make it convenient to r/w registers
    register unsigned long rdi asm ("rdi");
    register unsigned long rsi asm ("rsi");
    register unsigned long rdx asm ("rdx");
    register unsigned long rcx asm ("rcx");
    register unsigned long r8 asm ("r8");
    register unsigned long r9 asm ("r9");
    register unsigned long rbp asm ("rbp");

    unsigned long saved_rdi = rdi;
    unsigned long saved_rsi = rsi;
    unsigned long saved_rdx = rdx;
    unsigned long saved_rcx = rcx;
    unsigned long saved_r8 = r8;
    unsigned long saved_r9 = r9;

    unsigned char *real_main = (unsigned char *)saved_rdi;
    if(real_main == main_ptr)
    {
        // We set accurate stack trace here
        initial_stack = (unsigned char *)*(((unsigned long *)rbp + 3));
        // Clear possible garbage pushed into the stack
        stack_size_list_p->clear();

        // old rbp, return address to __libc_start_main, return address to _start, stack bottom
        // fprintf(stderr, "Accurate stack bottom = %p\n", initial_stack);
    }
    else
    {
        fprintf(stderr, "Did not find main(). Is this the real initializer?\n");
    }

    // Restore the first 5 bytes
    RestoreFunction(restore_array, (unsigned char *)libc_start_main_ptr);

    // And return to the first 5 bytes
    *(((unsigned long *)rbp + 1)) -= 5;

    // Enable this line to hijack main()
    //saved_rdi = (unsigned long)&m;

    rdi = saved_rdi;
    rsi = saved_rsi;
    rdx = saved_rdx;
    rcx = saved_rcx;
    r8 = saved_r8;
    r9 = saved_r9;

    reentry = false;

    return;
}

/*
 * InjectLibcStartMain() - Inject into __libc_start_main()'s first 5
 * bytes, and we could obtain some fundamental parameter of the program
 */
void InjectLibcStartMain()
{
    InjectFunction(INS_CALL,
                   libc_start_main_ptr,
                   (unsigned char *)&LibcStartMain,
                   restore_array);

    return;
}

/*
 * InitDataStructure() - Initialize data structure
 *
 * Since C++ compiler will not call constructor for globally
 * defined objects, we need to explicitly call them using operator new
 */
void InitDataStructure()
{
    object_map_p = new map<void *, MemObject>();
    stack_size_list_p = new vector<pair<unsigned long, unsigned long> >();
    alloc_count_p = new map<size_t, int>();
    mem_map_p = new map<void *, MemAlloc>();
    mem_size_list_p = new vector<size_t>();
    section_map_p = new map<string, SectionInfo>();
    lib_map_p = new map<string, MemObject>;

    return;
}

/*
 * InstallReadLibraryFunction() - Find real library function pointer and
 * save them for later use
 *
 * NOTE: This must be called in reentry mode or init mode, since
 * dlsym() will try to allocate 32 byte memory at first run
 */
void InstallRealLibraryFunction()
{
    real_malloc = (malloc_func_t)dlsym(RTLD_NEXT, "malloc");
    assert(real_malloc);

    real_free = (free_func_t)dlsym(RTLD_NEXT, "free");
    assert(real_free);

    real_realloc = (realloc_func_t)dlsym(RTLD_NEXT, "realloc");
    assert(real_realloc);

    // The starting routine after _start where stack boundaries are passed in
    libc_start_main_ptr = (unsigned char *)dlsym(RTLD_NEXT, "__libc_start_main");
    assert(libc_start_main_ptr);

    // fprintf(stderr, "&__libc_start_main = %p\n", libc_start_main_ptr);

    return;
}

/*
 * PrintAllocationStatistics() - As name suggests
 */
void PrintAllocationStatistics()
{
    fprintf(stderr, "==================== Memory Statistics ====================\n");

    fprintf(stderr, ">> Allocation Distribution\n");
    for(map<size_t, int>::iterator it = alloc_count_p->begin();
        it != alloc_count_p->end();
        it++)
    {
        fprintf(stderr, "   Size = %lu, count = %d\n", it->first, it->second);
    }

    fprintf(stderr, ">> Allocation Count\n");
    fprintf(stderr, "   Total malloc() = %d\n", malloc_count);
    fprintf(stderr, "   Total free() = %d\n", free_count);
    fprintf(stderr, "   Total realloc() = %d\n", realloc_count);

    fprintf(stderr, ">> Resource Consumption\n");
    fprintf(stderr, "   Maximum = %lu Bytes\n", max_allocated);
    fprintf(stderr, "   Current = %lu Bytes\n", current_allocated);
    fprintf(stderr, "   Total allocated = %lu Bytes\n", total_allocated);
    fprintf(stderr, "   Total freed = %lu Bytes\n", total_freed);
    fprintf(stderr, "   Max stack size = %lu Bytes\n", max_stack_size);

    assert(section_map_p->find(".data") != section_map_p->end());
    fprintf(stderr, "   Static data segment = %lu Bytes\n", (*section_map_p)[".data"].bytes);

    assert(section_map_p->find(".bss") != section_map_p->end());
    fprintf(stderr, "   BSS segment = %lu Bytes\n", (*section_map_p)[".bss"].bytes);

    assert(section_map_p->find(".rodata") != section_map_p->end());
    fprintf(stderr, "   Read-only data segment = %lu Bytes\n", (*section_map_p)[".rodata"].bytes);

    fprintf(stderr, ">> Leaked Memory (total %lu leaks)\n", mem_map_p->size());
    if(mem_map_p->size() > 0)
    {
        for(map<void *, MemAlloc>::iterator it = mem_map_p->begin();
            it != mem_map_p->end();
            it++)
        {
            fprintf(stderr, "   Base = %p, size = %lu\n", it->first, it->second.bytes);
            fprintf(stderr, "   \tFrom %s\n", it->second.name.c_str());
        }
    }
    else
    {
        fprintf(stderr, "   None");
    }

    return;
}

/*
 * Library Constructor - Allocate object and initialize
 */
__attribute__((constructor)) void _init()
{
    // After this line all mem allocation uses the temporary
    // stack based allocatior
    before_init = true;

    // Make sure memory is aligned
    simple_alloc_p = (unsigned char *)(((unsigned long)simple_alloc_p + 7) & (~0x7UL));

    InstallRealLibraryFunction();
    InjectStackProfiler();
    InjectLibcStartMain();

    // Before that every malloc and free uses simple_alloc
    // which is a stack based memory allocator in 4K Page
    before_init = false;

    // We temporarily disable malloc and free monitor
    reentry = true;

    // Initialize data structure
    InitDataStructure();

    ParseBinaryLayout(LIB_MAP_FILE_NAME, ParseGlibLayoutCallback);
    ParseBinaryLayout(MAP_FILE_NAME, ParseBinaryLayoutCallback);
    ParseSectionLayout(SEC_FILE_NAME);

    reentry = false;

    // This is an approximation
    initial_stack = (unsigned char *)__builtin_frame_address(0);
    // fprintf(stderr, "Initial stack pointer ~= %p\n", initial_stack);

    return;
}

/*
 * GetPeakMemUsage() - Returns memory statistics  
 */
extern "C" {
  size_t GetPeakMemUsage() {
    size_t data_size = (section_map_p->find(".data") != section_map_p->end()) ? \
                       ((*section_map_p)[".data"].bytes) : 0UL;
    size_t bss_size = (section_map_p->find(".bss") != section_map_p->end()) ? \
                      ((*section_map_p)[".bss"].bytes) : 0UL;

    size_t peak_usage = max_allocated + max_stack_size + data_size + bss_size;
    size_t max_map_size = max_malloc_count * (sizeof(std::pair<void *, MemAlloc>));
    /* return default base memory if the peak and max map size don't overlap */
    if (peak_usage < max_map_size)
        return 256000;
    else
        return peak_usage - max_map_size;
  }
}

/*
 * Destructor - Free up all string allocated during initialization
 */
__attribute__((destructor)) void _fini()
{
    // Put this before any delete happens
    // PrintAllocationStatistics();
    // printf("******** %lu\n", max_malloc_count * (sizeof(std::pair<void *, MemAlloc>)));

    reentry = true;
    
    // This is reused
    FILE *fp;

    fp = fopen(MEM_USAGE_FILE, "w");
    assert(fp);

    for(unsigned int i = 0;i < mem_size_list_p->size();i += 1)
    {
        // fprintf(fp, "%u %lu\n", i, (*mem_size_list_p)[i]);
    }

    fclose(fp);

    // Write leak file
    fp = fopen(MEM_LEAK_FILE, "w");
    assert(fp);

    for(map<void *, MemAlloc>::iterator it = mem_map_p->begin();
        it != mem_map_p->end();
        it++)
    {
        // unsigned long leak_time = it->second.counter;
        // fprintf(fp, "%lu %lu\n", leak_time, (*mem_size_list_p)[leak_time]);
    }

    fclose(fp);

    // Write allocation distribution file
    fp = fopen(ALLOC_DIST_FILE, "w");
    assert(fp);

    //int counter = 0;
    for(map<size_t, int>::iterator it = alloc_count_p->begin();
        it != alloc_count_p->end();
        it++)//, counter++)
    {
        // fprintf(fp, "%lu %d\n", it->first, it->second);
    }

    fclose(fp);

    fp = fopen(STACK_SIZE_FILE, "w");
    assert(fp);

    for(unsigned long i = 0;i < stack_size_list_p->size();i++)
    {
        /* fprintf(fp,
                "%lu %lu\n",
                stack_size_list_p->at(i).first,
                stack_size_list_p->at(i).second); */
    }

    fclose(fp);

    // Free the map
    delete object_map_p;
    delete alloc_count_p;
    delete mem_map_p;
    delete mem_size_list_p;
    delete stack_size_list_p;
    delete section_map_p;
    delete lib_map_p;

    reentry = false;
    return;
}

