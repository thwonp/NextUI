/////////////////////////////////////////////////////////////////////////////////////////

// File: common/generic_video.c
// Generic implementations of video functions, to be used by platforms that don't
// provide their own implementations.
// Used by: tg5050
// Library dependencies: SDL2, OpenGL (e.g. gles2), pthread, NEON
// Tool dependencies: none
// Script dependencies: none

// \note This files does not have an acompanying header, as all functions are declared in api.h
// with minimal fallback implementations
// \sa FALLBACK_IMPLEMENTATION

/////////////////////////////////////////////////////////////////////////////////////////

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define NEXTUI_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define NEXTUI_TSAN 1
#endif

static int finalScaleFilter=GL_LINEAR;
static int reloadShaderTextures = 1;
static int shaderResetRequested = 0;

static SDL_BlendMode getPremultipliedBlendMode(void) {
	return SDL_ComposeCustomBlendMode(
		SDL_BLENDFACTOR_ONE,
		SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
		SDL_BLENDOPERATION_ADD,
		SDL_BLENDFACTOR_ONE,
		SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
		SDL_BLENDOPERATION_ADD
	);
}

// shader stuff

typedef struct Shader {
	int srcw;
	int srch;
	int texw;
	int texh;
	int filter;
	GLuint shader_p;
	int scale;
	int srctype;
	int scaletype;
	char *filename;
	GLuint texture;
	int updated;
	GLint u_FrameDirection;
	GLint u_FrameCount;
	GLint u_OutputSize;
	GLint u_TextureSize;
	GLint u_InputSize;
	GLint OrigInputSize;
	GLint texLocation;
	GLint texelSizeLocation;
	ShaderParam *pragmas;  // Dynamic array of parsed pragma parameters
	int num_pragmas;       // Count of valid pragma parameters

} Shader;

GLuint g_shader_default = 0;
GLuint g_shader_overlay = 0;
GLuint g_noshader = 0;

Shader* shaders[MAXSHADERS] = {
    &(Shader){ .shader_p = 0, .scale = 1, .filter = GL_LINEAR, .scaletype = 1, .srctype = 0, .filename ="stock.glsl", .texture = 0, .updated = 1 },
    &(Shader){ .shader_p = 0, .scale = 1, .filter = GL_LINEAR, .scaletype = 1, .srctype = 0, .filename ="stock.glsl", .texture = 0, .updated = 1 },
    &(Shader){ .shader_p = 0, .scale = 1, .filter = GL_LINEAR, .scaletype = 1, .srctype = 0, .filename ="stock.glsl", .texture = 0, .updated = 1 },
};

static int nrofshaders = 0; // choose between 1 and 3 pipelines, > pipelines = more cpu usage, but more shader options and shader upscaling stuff

///////////////////////////////

static struct VID_Context {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* target_layer1;
	SDL_Texture* target_layer2;
	SDL_Texture* stream_layer1;
	SDL_Texture* target_layer3;
	SDL_Texture* target_layer4;
	SDL_Texture* target_layer5;
	SDL_Texture* target;
	SDL_Texture* effect;
	SDL_Texture* overlay;
	SDL_Surface* screen;
	SDL_GLContext gl_context;

	GFX_Renderer* blit; // yeesh
	int width;
	int height;
	int pitch;
	int sharpness;
	uint32_t clear_color;
} vid;

static int device_width;
static int device_height;
static int device_pitch;
static uint32_t SDL_transparentBlack = 0;

#define OVERLAYS_FOLDER SDCARD_PATH "/Overlays"

static char* overlay_path = NULL;

// Notification overlay state for RA achievements
typedef struct {
    SDL_Surface* surface;
    int x;
    int y;
    int dirty;
    GLuint tex;
    int tex_w, tex_h;
    int clear_frames;  // Frames to clear framebuffer after notification ends
} NotificationOverlay;

static NotificationOverlay notif = {0};

void PLAT_setNotificationSurface(SDL_Surface* surface, int x, int y) {
    notif.surface = surface;
    notif.x = x;
    notif.y = y;
    notif.dirty = 1;
}

void PLAT_clearNotificationSurface(void) {
    notif.surface = NULL;
    notif.dirty = 0;  // Nothing to update since surface is NULL
    notif.clear_frames = 3;  // Clear for 3 frames (triple buffering safety)
}


#define MAX_SHADERLINE_LENGTH 512
int extractPragmaParameters(const char *shaderSource, ShaderParam *params, int maxParams) {
    const char *pragmaPrefix = "#pragma parameter";
    char line[MAX_SHADERLINE_LENGTH];
    int paramCount = 0;

    const char *currentPos = shaderSource;

    while (*currentPos && paramCount < maxParams) {
        int i = 0;

        // Read a line
        while (*currentPos && *currentPos != '\n' && i < MAX_SHADERLINE_LENGTH - 1) {
            line[i++] = *currentPos++;
        }
        line[i] = '\0';
        if (*currentPos == '\n') currentPos++;

        // Check if it's a #pragma parameter line
        if (strncmp(line, pragmaPrefix, strlen(pragmaPrefix)) == 0) {
            const char *start = line + strlen(pragmaPrefix);
            while (*start == ' ') start++;

            ShaderParam *p = &params[paramCount];

            // Try to parse
            if (sscanf(start, "%127s \"%127[^\"]\" %f %f %f %f",
                       p->name, p->label, &p->def, &p->min, &p->max, &p->step) == 6) {
                paramCount++;
            } else {
                fprintf(stderr, "Failed to parse line:\n%s\n", line);
            }
        }
    }

    return paramCount; // number of parameters found
}

GLuint link_program(GLuint vertex_shader, GLuint fragment_shader, const char* cache_key, const char* source_path) {
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), SDCARD_PATH "/.shadercache/%s.bin", cache_key);

    GLuint program = glCreateProgram();
    GLint success;

    // Try to load cached binary first, unless source is newer than the cache.
    // Prefer recompile on any stat failure (missing cache, missing source, NULL source_path).
    int use_cache = 0;
    if (source_path != NULL) {
        struct stat src_st, bin_st;
        if (stat(source_path, &src_st) == 0 && stat(cache_path, &bin_st) == 0) {
            use_cache = (src_st.st_mtime <= bin_st.st_mtime);
        }
    }
    FILE *f = use_cache ? fopen(cache_path, "rb") : NULL;
    if (f) {
        GLint binaryFormat;
        fread(&binaryFormat, sizeof(GLint), 1, f);
        fseek(f, 0, SEEK_END);
        size_t length = ftell(f) - sizeof(GLint);
        fseek(f, sizeof(GLint), SEEK_SET);
        void *binary = malloc(length);
        fread(binary, 1, length, f);
        fclose(f);

        glProgramBinary(program, binaryFormat, binary, length);
        free(binary);

        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (success) {
            LOG_info("Loaded shader program from cache: %s\n", cache_key);
            return program;
        } else {
            LOG_info("Cache load failed, falling back to compile.\n");
            glDeleteProgram(program);
            program = glCreateProgram();
        }
    }

    // Compile and link if cache failed
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success) {
        GLint logLength;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        char* log = (char*)malloc(logLength);
        glGetProgramInfoLog(program, logLength, &logLength, log);
        LOG_error("Program link error: %s\n", log);
        free(log);
        return program;
    }

    GLint binaryLength;
    GLenum binaryFormat;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
    void* binary = malloc(binaryLength);
    glGetProgramBinary(program, binaryLength, NULL, &binaryFormat, binary);

    mkdir(SDCARD_PATH "/.shadercache", 0755);
    f = fopen(cache_path, "wb");
    if (f) {
        fwrite(&binaryFormat, sizeof(GLenum), 1, f);
        fwrite(binary, 1, binaryLength, f);
        fclose(f);
        LOG_info("Saved shader program to cache: %s\n", cache_key);
    }
    free(binary);

    LOG_info("Program linked and cached\n");
    return program;
}

char* load_shader_source(const char* filename) {
	char filepath[256];
	snprintf(filepath, sizeof(filepath), "%s", filename);
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open shader file: %s\n", filepath);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    rewind(file);

    char* source = (char*)malloc(length + 1);
    if (!source) {
        fprintf(stderr, "Memory allocation failed\n");
        fclose(file);
        return NULL;
    }

    fread(source, 1, length, file);
    source[length] = '\0';
    fclose(file);
    return source;
}

GLuint load_shader_from_file(GLenum type, const char* filename, const char* path) {
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", path, filename);
    char* source = load_shader_source(filepath);
    if (!source) return 0;

    LOG_info("load shader from file %s\n", filepath);

    // Filter out lines starting with "#pragma parameter"
    char* cleaned = malloc(strlen(source) + 1);
    if (!cleaned) {
        fprintf(stderr, "Out of memory\n");
        free(source);
        return 0;
    }
    cleaned[0] = '\0';

    char* line = strtok(source, "\n");
    while (line) {
        if (strncmp(line, "#pragma parameter", 17) != 0) {
            strcat(cleaned, line);
            strcat(cleaned, "\n");
        }
        line = strtok(NULL, "\n");
    }

    const char* define = NULL;
    const char* default_precision = NULL;
    if (type == GL_VERTEX_SHADER) {
        define = "#define VERTEX\n";
    } else if (type == GL_FRAGMENT_SHADER) {
        define = "#define FRAGMENT\n";
        default_precision =
            "#ifdef GL_ES\n"
            // compat fix for fwidth, dFdx, dFdy
            "#ifdef GL_OES_standard_derivatives\n"
            "#extension GL_OES_standard_derivatives : enable\n"
            "#endif\n"
            "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
            "precision highp float;\n"
            "#else\n"
            "precision mediump float;\n"
            "#endif\n"
            "#endif\n"
            "#define PARAMETER_UNIFORM\n";
    } else {
        fprintf(stderr, "Unsupported shader type\n");
        free(source);
        free(cleaned);
        return 0;
    }

    const char* version_start = strstr(cleaned, "#version");
    const char* version_end = version_start ? strchr(version_start, '\n') : NULL;

    const char* replacement_version = "#version 300 es\n";
    const char* fallback_version = "#version 100\n";

    char* combined = NULL;
    size_t define_len = strlen(define);
    size_t precision_len = default_precision ? strlen(default_precision) : 0;
    size_t source_len = strlen(cleaned);
    size_t combined_len = 0;

    int should_replace_with_300es = 0;
    if (version_start && version_end) {
        char version_str[32] = {0};
        size_t len = version_end - version_start;
        if (len < sizeof(version_str)) {
            strncpy(version_str, version_start, len);
            version_str[len] = '\0';

            if (
                strstr(version_str, "#version 110") ||
                strstr(version_str, "#version 120") ||
                strstr(version_str, "#version 130") ||
                strstr(version_str, "#version 140") ||
                strstr(version_str, "#version 150") ||
                strstr(version_str, "#version 330") ||
                strstr(version_str, "#version 400") ||
                strstr(version_str, "#version 410") ||
                strstr(version_str, "#version 420") ||
                strstr(version_str, "#version 430") ||
                strstr(version_str, "#version 440") ||
                strstr(version_str, "#version 450")
            ) {
                should_replace_with_300es = 1;
            }
        }
    }

    if (version_start && version_end && should_replace_with_300es) {
        size_t header_len = version_end - cleaned + 1;
        size_t version_len = strlen(replacement_version);
        combined_len = version_len + define_len + precision_len + (source_len - header_len) + 1;
        combined = (char*)malloc(combined_len);
        if (!combined) {
            fprintf(stderr, "Out of memory\n");
            free(source);
            free(cleaned);
            return 0;
        }

        strcpy(combined, replacement_version);
        strcat(combined, define);
        if (default_precision) strcat(combined, default_precision);
        strcat(combined, cleaned + header_len);
    } else if (version_start && version_end) {
        size_t header_len = version_end - cleaned + 1;
        combined_len = header_len + define_len + precision_len + (source_len - header_len) + 1;
        combined = (char*)malloc(combined_len);
        if (!combined) {
            fprintf(stderr, "Out of memory\n");
            free(source);
            free(cleaned);
            return 0;
        }

        memcpy(combined, cleaned, header_len);
        memcpy(combined + header_len, define, define_len);
        if (default_precision)
            memcpy(combined + header_len + define_len, default_precision, precision_len);
        strcpy(combined + header_len + define_len + precision_len, cleaned + header_len);
    } else {
        size_t version_len = strlen(fallback_version);
        combined_len = version_len + define_len + precision_len + source_len + 1;
        combined = (char*)malloc(combined_len);
        if (!combined) {
            fprintf(stderr, "Out of memory\n");
            free(source);
            free(cleaned);
            return 0;
        }

        strcpy(combined, fallback_version);
        strcat(combined, define);
        if (default_precision) strcat(combined, default_precision);
        strcat(combined, cleaned);
    }

    GLuint shader = glCreateShader(type);
    const char* combined_ptr = combined;
    glShaderSource(shader, 1, &combined_ptr, NULL);
    glCompileShader(shader);

    free(source);
    free(cleaned);
    free(combined);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compilation failed:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

void PLAT_initShaders() {
	SDL_GL_MakeCurrent(vid.window, vid.gl_context);
	glViewport(0, 0, device_width, device_height);

	GLuint vertex;
	GLuint fragment;

	char sysshader_path[512];

	// Final  display shader (simple texture blit)
	vertex = load_shader_from_file(GL_VERTEX_SHADER, "default.glsl",SYSSHADERS_FOLDER);
	fragment = load_shader_from_file(GL_FRAGMENT_SHADER, "default.glsl",SYSSHADERS_FOLDER);
	snprintf(sysshader_path, sizeof(sysshader_path), SYSSHADERS_FOLDER "/default.glsl");
	g_shader_default = link_program(vertex, fragment, "default.glsl", sysshader_path);

	// Overlay shader, for png overlays and static line/grid overlays
	vertex = load_shader_from_file(GL_VERTEX_SHADER, "overlay.glsl",SYSSHADERS_FOLDER);
	fragment = load_shader_from_file(GL_FRAGMENT_SHADER, "overlay.glsl",SYSSHADERS_FOLDER);
	snprintf(sysshader_path, sizeof(sysshader_path), SYSSHADERS_FOLDER "/overlay.glsl");
	g_shader_overlay = link_program(vertex, fragment, "overlay.glsl", sysshader_path);

	// Stand-In if a shader is supposed to be applied, but wasnt compiled properly (shaper_p == NULL)
	vertex = load_shader_from_file(GL_VERTEX_SHADER, "noshader.glsl",SYSSHADERS_FOLDER);
	fragment = load_shader_from_file(GL_FRAGMENT_SHADER, "noshader.glsl",SYSSHADERS_FOLDER);
	snprintf(sysshader_path, sizeof(sysshader_path), SYSSHADERS_FOLDER "/noshader.glsl");
	g_noshader = link_program(vertex, fragment, "noshader.glsl", sysshader_path);

	LOG_info("default shaders loaded, %i\n\n",g_shader_default);
}

void PLAT_initNotificationTexture(void) {
	// Pre-allocate notification texture to avoid frame skip on first notification
	glGenTextures(1, &notif.tex);
	glBindTexture(GL_TEXTURE_2D, notif.tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	// Allocate full-screen texture with transparent pixels
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, device_width, device_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	notif.tex_w = device_width;
	notif.tex_h = device_height;
}


static void sdl_log_stdout(
    void *userdata,
    int category,
    SDL_LogPriority priority,
    const char *message)
{
    (void)userdata;
    (void)category;
    (void)priority;

    LOG_info("[SDL] %s\n", message);
}

void PLAT_resetShaders() {
	reloadShaderTextures = 1;
	shaderResetRequested = 1;
}

SDL_Surface* PLAT_initVideo(void) {
	LOG_info("PLAT_initVideo: entering\n");
	gfx_flush_state_reset(); /* clear EGL flush counter on every video init */
	sync();

#if NEXTUI_TSAN
	/*
	 * Mesa's llvmpipe spawns worker threads that race during teardown under TSAN.
	 * Softpipe keeps rendering single-threaded, avoiding the contested mutex/cond
	 * destruction without affecting release builds.
	 */
	setenv("GALLIUM_DRIVER", "softpipe", 0);
	setenv("LP_NUM_THREADS", "1", 1);
#endif
	SDL_LogSetOutputFunction(sdl_log_stdout, NULL);
	//SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);
	SDL_InitSubSystem(SDL_INIT_VIDEO);
	LOG_info("PLAT_initVideo: SDL_InitSubSystem done\n");
	sync();
	{
		SDL_DisplayMode mode;
		SDL_GetCurrentDisplayMode(0, &mode);
	}
	SDL_ShowCursor(0);

//	SDL_version compiled;
//	SDL_version linked;
//	SDL_VERSION(&compiled);
//	SDL_GetVersion(&linked);
//	LOG_info("Compiled SDL version %d.%d.%d ...\n", compiled.major, compiled.minor, compiled.patch);
//	LOG_info("Linked SDL version %d.%d.%d.\n", linked.major, linked.minor, linked.patch);
//		LOG_info("Available video drivers:\n");
//	for (int i=0; i<SDL_GetNumVideoDrivers(); i++) {
//		LOG_info("- %s\n", SDL_GetVideoDriver(i));
//	}
//	LOG_info("Current video driver: %s\n", SDL_GetCurrentVideoDriver());
//	LOG_info("Available render drivers:\n");
//	for (int i=0; i<SDL_GetNumRenderDrivers(); i++) {
//		SDL_RendererInfo info;
//		SDL_GetRenderDriverInfo(i,&info);
//		LOG_info("- %s\n", info.name);
//	}
//	LOG_info("Available video displays: %d\n", SDL_GetNumVideoDisplays());
//	LOG_info("Available display modes:\n");
//	SDL_DisplayMode mode;
//	for (int i=0; i<SDL_GetNumDisplayModes(0); i++) {
//		SDL_GetDisplayMode(0, i, &mode);
//		LOG_info("- %ix%i (%s)\n", mode.w,mode.h, SDL_GetPixelFormatName(mode.format));
//	}
//	SDL_GetCurrentDisplayMode(0, &mode);
//	LOG_info("Current display mode: %ix%i (%s)\n", mode.w,mode.h, SDL_GetPixelFormatName(mode.format));

	int w = FIXED_WIDTH;
	int h = FIXED_HEIGHT;
	int p = FIXED_PITCH;

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"1");
	SDL_SetHint(SDL_HINT_RENDER_DRIVER,"opengl");
	SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION,"1");

	// Tell EGL to create a GLES2 context BEFORE window creation.
	// Without this, SDL2/EGL defaults to requesting desktop GL on some MALI builds.
#ifdef USE_GLES
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#endif

	int win_w = w;
	int win_h = h;
	vid.window   = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, win_w, win_h, SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
	LOG_info("PLAT_initVideo: SDL_CreateWindow done\n");
	sync();
	LOG_info("video driver: %s\n", SDL_GetCurrentVideoDriver());
	LOG_info("render drivers (%d):\n", SDL_GetNumRenderDrivers());
	for (int i = 0; i < SDL_GetNumRenderDrivers(); i++) {
		SDL_RendererInfo rinfo;
		SDL_GetRenderDriverInfo(i, &rinfo);
		LOG_info("  [%d] %s\n", i, rinfo.name);
	}
	sync();
	vid.renderer = SDL_CreateRenderer(vid.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	LOG_info("PLAT_initVideo: SDL_CreateRenderer done\n");
	sync();
#if defined(USE_GLES)
	/* On platforms where SDL's GLES renderer loads libGLESv2 with RTLD_LOCAL,
	 * PLT entries for gl* symbols in the main binary cannot be resolved lazily.
	 * Re-open with RTLD_GLOBAL after SDL_CreateRenderer so the global namespace
	 * sees the already-loaded library. Harmless if already RTLD_GLOBAL or absent. */
	dlopen("libGLESv2.so.2", RTLD_GLOBAL | RTLD_LAZY);
#endif
	SDL_SetRenderDrawBlendMode(vid.renderer, SDL_BLENDMODE_BLEND);
	SDL_RendererInfo info;
	SDL_GetRendererInfo(vid.renderer, &info);
	LOG_info("Current render driver: %s\n", info.name);
	// print texture formats
	LOG_info("Supported texture formats:\n");
	for (Uint32 i=0; i<info.num_texture_formats; i++) {
		LOG_info("- %s\n", SDL_GetPixelFormatName(info.texture_formats[i]));
	}

	if(strcmp("Desktop", PLAT_getModel()) == 0) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	}
	else {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	}

	vid.gl_context = SDL_GL_CreateContext(vid.window);
	LOG_info("PLAT_initVideo: SDL_GL_CreateContext done, ctx=%p\n", (void*)vid.gl_context);
	sync();
	SDL_GL_MakeCurrent(vid.window, vid.gl_context);
	glViewport(0, 0, w, h);
	LOG_info("PLAT_initVideo: glViewport done\n");
	sync();

	/* ---- TEMPORARY DIAGNOSTIC: removed in task 004 ---- */
#ifdef PLATFORM_zero28
	/*
	 * Dump EGL display/surface/config attributes so we can determine the
	 * PowerVR driver's buffer queue depth and available swap extensions.
	 * All EGL symbols are resolved via dlsym because libEGL is loaded
	 * dynamically by SDL2 (NEEDED stripped post-link; direct linkage breaks
	 * SDL init). SDL2 dlopen()s libEGL with RTLD_LOCAL, so RTLD_DEFAULT
	 * cannot see its symbols; we acquire an explicit handle via dlopen()
	 * with RTLD_NOLOAD (no second copy loaded) and resolve through that.
	 */
	{
		/* --- EGL type aliases (avoid pulling in EGL headers) --- */
		typedef void   *EGLDisplay_t;
		typedef void   *EGLSurface_t;
		typedef void   *EGLContext_t;
		typedef void   *EGLConfig_t;
		typedef int     EGLint_t;
		typedef unsigned int EGLBoolean_t;
#define EGL_TRUE_  1
#define EGL_DRAW_  0x3059
		/* EGL_CONFIG_ID and surface/config attribs */
#define EGL_CONFIG_ID_          0x3028
#define EGL_BUFFER_SIZE_        0x3020
#define EGL_RED_SIZE_           0x3024
#define EGL_GREEN_SIZE_         0x3023
#define EGL_BLUE_SIZE_          0x3022
#define EGL_ALPHA_SIZE_         0x3021
#define EGL_DEPTH_SIZE_         0x3025
#define EGL_STENCIL_SIZE_       0x3026
#define EGL_SAMPLES_            0x3031
#define EGL_SURFACE_TYPE_       0x3033
#define EGL_RENDERABLE_TYPE_    0x3040
#define EGL_CONFIG_CAVEAT_      0x3027
#define EGL_NATIVE_RENDERABLE_  0x302D
#define EGL_RENDER_BUFFER_      0x3086
#define EGL_SWAP_BEHAVIOR_      0x3093
#define EGL_WIDTH_              0x3057
#define EGL_HEIGHT_             0x3056
#define EGL_MULTISAMPLE_RESOLVE_ 0x3099
		/* eglQueryString names */
#define EGL_VERSION_STR_     0x3054
#define EGL_VENDOR_STR_      0x3053
#define EGL_CLIENT_APIS_STR_ 0x308D
#define EGL_EXTENSIONS_STR_  0x3055

		/* --- acquire a handle to the already-loaded libEGL ---
		 * Try RTLD_NOLOAD first (returns handle without loading a second
		 * copy); fall back to a plain RTLD_LAZY load as last resort.
		 * Do not call dlerror() until all three attempts have failed. */
		void *egl_handle = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
		if (egl_handle) {
			LOG_info("  EGL handle via dlopen(\"libEGL.so.1\", NOLOAD)\n");
		} else {
			egl_handle = dlopen("libEGL.so", RTLD_LAZY | RTLD_NOLOAD);
			if (egl_handle) {
				LOG_info("  EGL handle via dlopen(\"libEGL.so\", NOLOAD)\n");
			} else {
				egl_handle = dlopen("libEGL.so.1", RTLD_LAZY);
				if (egl_handle) {
					LOG_info("  EGL handle via dlopen(\"libEGL.so.1\", RTLD_LAZY)\n");
				}
			}
		}

		/* --- resolve function pointers --- */
		EGLDisplay_t (*p_eglGetCurrentDisplay)(void)           = egl_handle ? dlsym(egl_handle, "eglGetCurrentDisplay") : NULL;
		EGLSurface_t (*p_eglGetCurrentSurface)(EGLint_t)       = egl_handle ? dlsym(egl_handle, "eglGetCurrentSurface") : NULL;
		EGLContext_t (*p_eglGetCurrentContext)(void)            = egl_handle ? dlsym(egl_handle, "eglGetCurrentContext") : NULL;
		const char * (*p_eglQueryString)(EGLDisplay_t, EGLint_t) = egl_handle ? dlsym(egl_handle, "eglQueryString") : NULL;
		EGLBoolean_t (*p_eglQueryContext)(EGLDisplay_t, EGLContext_t, EGLint_t, EGLint_t *) = egl_handle ? dlsym(egl_handle, "eglQueryContext") : NULL;
		EGLBoolean_t (*p_eglQuerySurface)(EGLDisplay_t, EGLSurface_t, EGLint_t, EGLint_t *) = egl_handle ? dlsym(egl_handle, "eglQuerySurface") : NULL;
		EGLBoolean_t (*p_eglGetConfigAttrib)(EGLDisplay_t, EGLConfig_t, EGLint_t, EGLint_t *) = egl_handle ? dlsym(egl_handle, "eglGetConfigAttrib") : NULL;
		/* eglGetConfigs: needed to convert config-id -> EGLConfig handle */
		EGLBoolean_t (*p_eglGetConfigs)(EGLDisplay_t, EGLConfig_t *, EGLint_t, EGLint_t *) = egl_handle ? dlsym(egl_handle, "eglGetConfigs") : NULL;
		EGLint_t     (*p_eglGetError)(void)                    = egl_handle ? dlsym(egl_handle, "eglGetError") : NULL;

		LOG_info("===== EGL CONFIG =====\n");

		/* Verify all symbols resolved */
		int egl_ok = 1;
		if (!egl_handle) {
			LOG_info("  EGL handle not obtainable; skipping EGL dump\n");
			egl_ok = 0;
		}
		if (egl_ok && !p_eglGetCurrentDisplay)   { LOG_info("  EGL dlsym FAILED: eglGetCurrentDisplay\n");   egl_ok = 0; }
		if (egl_ok && !p_eglGetCurrentSurface)   { LOG_info("  EGL dlsym FAILED: eglGetCurrentSurface\n");   egl_ok = 0; }
		if (egl_ok && !p_eglGetCurrentContext)   { LOG_info("  EGL dlsym FAILED: eglGetCurrentContext\n");    egl_ok = 0; }
		if (egl_ok && !p_eglQueryString)         { LOG_info("  EGL dlsym FAILED: eglQueryString\n");          egl_ok = 0; }
		if (egl_ok && !p_eglQueryContext)        { LOG_info("  EGL dlsym FAILED: eglQueryContext\n");          egl_ok = 0; }
		if (egl_ok && !p_eglQuerySurface)        { LOG_info("  EGL dlsym FAILED: eglQuerySurface\n");         egl_ok = 0; }
		if (egl_ok && !p_eglGetConfigAttrib)     { LOG_info("  EGL dlsym FAILED: eglGetConfigAttrib\n");      egl_ok = 0; }
		if (egl_ok && !p_eglGetConfigs)          { LOG_info("  EGL dlsym FAILED: eglGetConfigs\n");           egl_ok = 0; }
		if (egl_ok && !p_eglGetError)            { LOG_info("  EGL dlsym FAILED: eglGetError\n");             egl_ok = 0; }

		if (!egl_ok) {
			LOG_info("  One or more EGL symbols not found; skipping EGL dump\n");
			LOG_info("===== /EGL CONFIG =====\n");
			goto egl_dump_done;
		}

		EGLDisplay_t dpy = p_eglGetCurrentDisplay();
		EGLSurface_t sfc = p_eglGetCurrentSurface(EGL_DRAW_);
		EGLContext_t ctx = p_eglGetCurrentContext();

		if (!dpy || !sfc || !ctx) {
			LOG_info("  EGL context not current: dpy=%p sfc=%p ctx=%p\n",
			         dpy, sfc, ctx);
			LOG_info("===== /EGL CONFIG =====\n");
			goto egl_dump_done;
		}

		/* --- EGL string attributes --- */
		const char *egl_version = p_eglQueryString(dpy, EGL_VERSION_STR_);
		const char *egl_vendor  = p_eglQueryString(dpy, EGL_VENDOR_STR_);
		const char *egl_apis    = p_eglQueryString(dpy, EGL_CLIENT_APIS_STR_);
		const char *egl_exts    = p_eglQueryString(dpy, EGL_EXTENSIONS_STR_);
		LOG_info("  EGL_VERSION:     %s\n", egl_version  ? egl_version  : "(null)");
		LOG_info("  EGL_VENDOR:      %s\n", egl_vendor   ? egl_vendor   : "(null)");
		LOG_info("  EGL_CLIENT_APIS: %s\n", egl_apis     ? egl_apis     : "(null)");
		/* extensions can be long; print in one shot */
		LOG_info("  EGL_EXTENSIONS:  %s\n", egl_exts     ? egl_exts     : "(null)");

		/* --- Extension presence check --- */
#define EGL_HAS_EXT(name) ((egl_exts && strstr(egl_exts, (name))) ? "yes" : "no")
		LOG_info("  ext EGL_KHR_swap_buffers_with_damage:    %s\n", EGL_HAS_EXT("EGL_KHR_swap_buffers_with_damage"));
		LOG_info("  ext EGL_EXT_swap_buffers_with_damage:    %s\n", EGL_HAS_EXT("EGL_EXT_swap_buffers_with_damage"));
		LOG_info("  ext EGL_ANDROID_front_buffer_auto_refresh: %s\n", EGL_HAS_EXT("EGL_ANDROID_front_buffer_auto_refresh"));
		LOG_info("  ext EGL_NV_post_sub_buffer:              %s\n", EGL_HAS_EXT("EGL_NV_post_sub_buffer"));
		LOG_info("  ext EGL_IMG_context_priority:            %s\n", EGL_HAS_EXT("EGL_IMG_context_priority"));
		LOG_info("  ext EGL_KHR_partial_update:              %s\n", EGL_HAS_EXT("EGL_KHR_partial_update"));
#undef EGL_HAS_EXT

		/* --- Resolve the EGLConfig handle from the current context's config-id --- */
		EGLint_t config_id = 0;
		if (p_eglQueryContext(dpy, ctx, EGL_CONFIG_ID_, &config_id) != EGL_TRUE_) {
			LOG_info("  eglQueryContext(EGL_CONFIG_ID) failed: 0x%x\n", p_eglGetError());
			goto surface_attribs;
		}
		LOG_info("  EGL_CONFIG_ID: %d\n", config_id);

		/* Walk the config list to find the matching handle */
		EGLint_t num_configs = 0;
		p_eglGetConfigs(dpy, NULL, 0, &num_configs);
		EGLConfig_t egl_cfg = NULL;
		if (num_configs > 0) {
			EGLConfig_t *cfgs = malloc((size_t)num_configs * sizeof(EGLConfig_t));
			if (cfgs) {
				EGLint_t got = 0;
				p_eglGetConfigs(dpy, cfgs, num_configs, &got);
				for (EGLint_t ci = 0; ci < got; ci++) {
					EGLint_t cid = 0;
					if (p_eglGetConfigAttrib(dpy, cfgs[ci], EGL_CONFIG_ID_, &cid) == EGL_TRUE_
					    && cid == config_id) {
						egl_cfg = cfgs[ci];
						break;
					}
				}
				free(cfgs);
			}
		}

		if (!egl_cfg) {
			LOG_info("  Could not find EGLConfig handle for config_id=%d\n", config_id);
			goto surface_attribs;
		}

		/* Helper macro: query one config attrib and log name + value (or error) */
#define LOG_CFG(attr_name, attr_val) \
		do { \
			EGLint_t _v = 0; \
			if (p_eglGetConfigAttrib(dpy, egl_cfg, (attr_val), &_v) == EGL_TRUE_) \
				LOG_info("  cfg %-30s = %d (0x%x)\n", (attr_name), _v, (unsigned)_v); \
			else \
				LOG_info("  cfg %-30s ERROR 0x%x\n", (attr_name), p_eglGetError()); \
		} while (0)

		LOG_CFG("EGL_BUFFER_SIZE",    EGL_BUFFER_SIZE_);
		LOG_CFG("EGL_RED_SIZE",       EGL_RED_SIZE_);
		LOG_CFG("EGL_GREEN_SIZE",     EGL_GREEN_SIZE_);
		LOG_CFG("EGL_BLUE_SIZE",      EGL_BLUE_SIZE_);
		LOG_CFG("EGL_ALPHA_SIZE",     EGL_ALPHA_SIZE_);
		LOG_CFG("EGL_DEPTH_SIZE",     EGL_DEPTH_SIZE_);
		LOG_CFG("EGL_STENCIL_SIZE",   EGL_STENCIL_SIZE_);
		LOG_CFG("EGL_SAMPLES",        EGL_SAMPLES_);
		LOG_CFG("EGL_SURFACE_TYPE",   EGL_SURFACE_TYPE_);
		LOG_CFG("EGL_RENDERABLE_TYPE",EGL_RENDERABLE_TYPE_);
		LOG_CFG("EGL_CONFIG_CAVEAT",  EGL_CONFIG_CAVEAT_);
		LOG_CFG("EGL_NATIVE_RENDERABLE", EGL_NATIVE_RENDERABLE_);
#undef LOG_CFG

		/* --- Surface attributes --- */
surface_attribs:;
		/* Helper macro: query one surface attrib and log name + value (or error) */
#define LOG_SFC(attr_name, attr_val) \
		do { \
			EGLint_t _v = 0; \
			if (p_eglQuerySurface(dpy, sfc, (attr_val), &_v) == EGL_TRUE_) \
				LOG_info("  sfc %-30s = %d (0x%x)\n", (attr_name), _v, (unsigned)_v); \
			else \
				LOG_info("  sfc %-30s ERROR 0x%x\n", (attr_name), p_eglGetError()); \
		} while (0)

		LOG_SFC("EGL_RENDER_BUFFER",       EGL_RENDER_BUFFER_);
		LOG_SFC("EGL_SWAP_BEHAVIOR",        EGL_SWAP_BEHAVIOR_);
		LOG_SFC("EGL_WIDTH",                EGL_WIDTH_);
		LOG_SFC("EGL_HEIGHT",               EGL_HEIGHT_);
		LOG_SFC("EGL_MULTISAMPLE_RESOLVE",  EGL_MULTISAMPLE_RESOLVE_);
#undef LOG_SFC

		LOG_info("===== /EGL CONFIG =====\n");
egl_dump_done:;
		/* clean up local macro namespace */
#undef EGL_TRUE_
#undef EGL_DRAW_
#undef EGL_CONFIG_ID_
#undef EGL_BUFFER_SIZE_
#undef EGL_RED_SIZE_
#undef EGL_GREEN_SIZE_
#undef EGL_BLUE_SIZE_
#undef EGL_ALPHA_SIZE_
#undef EGL_DEPTH_SIZE_
#undef EGL_STENCIL_SIZE_
#undef EGL_SAMPLES_
#undef EGL_SURFACE_TYPE_
#undef EGL_RENDERABLE_TYPE_
#undef EGL_CONFIG_CAVEAT_
#undef EGL_NATIVE_RENDERABLE_
#undef EGL_RENDER_BUFFER_
#undef EGL_SWAP_BEHAVIOR_
#undef EGL_WIDTH_
#undef EGL_HEIGHT_
#undef EGL_MULTISAMPLE_RESOLVE_
#undef EGL_VERSION_STR_
#undef EGL_VENDOR_STR_
#undef EGL_CLIENT_APIS_STR_
#undef EGL_EXTENSIONS_STR_
	}
#endif /* PLATFORM_zero28 */
	/* ---- END TEMPORARY DIAGNOSTIC ---- */

	vid.stream_layer1 = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w,h);
	vid.target_layer1 = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET , w,h);
	vid.target_layer2 = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET , w,h);
	vid.target_layer3 = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET , w,h);
	vid.target_layer4 = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET , w,h);
	vid.target_layer5 = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET , w,h);

	vid.target	= NULL; // only needed for non-native sizes

	vid.screen = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);

	SDL_BlendMode premult = getPremultipliedBlendMode();
	SDL_SetSurfaceBlendMode(vid.screen, SDL_BLENDMODE_BLEND);
	SDL_SetTextureBlendMode(vid.stream_layer1, premult);
	SDL_SetTextureBlendMode(vid.target_layer2, premult);
	SDL_SetTextureBlendMode(vid.target_layer3, SDL_BLENDMODE_BLEND); // straight alpha game art
	SDL_SetTextureBlendMode(vid.target_layer4, premult);
	SDL_SetTextureBlendMode(vid.target_layer5, SDL_BLENDMODE_BLEND);
	LOG_info("PLAT_initVideo: texture setup done\n");
	sync();

	vid.width	= w;
	vid.height	= h;
	vid.pitch	= p;

	SDL_transparentBlack = SDL_MapRGBA(vid.screen->format, 0, 0, 0, 0);
	PLAT_setClearColor(SDL_transparentBlack);

	device_width	= w;
	device_height	= h;
	device_pitch	= p;

	vid.sharpness = SHARPNESS_SOFT;

	return vid.screen;
}

void PLAT_setClearColor(uint32_t color) {
	vid.clear_color = color;
}

#define MAX_SHADER_PRAGMAS 32
void loadShaderPragmas(Shader *shader, const char *shaderSource) {
	shader->pragmas = calloc(MAX_SHADER_PRAGMAS, sizeof(ShaderParam));
	if (!shader->pragmas) {
		fprintf(stderr, "Out of memory allocating pragmas for %s\n", shader->filename);
		return;
	}
	shader->num_pragmas = extractPragmaParameters(shaderSource, shader->pragmas, MAX_SHADER_PRAGMAS);
}

ShaderParam* PLAT_getShaderPragmas(int i) {
    return shaders[i]->pragmas;
}

void PLAT_updateShader(int i, const char *filename, int *scale, int *filter, int *scaletype, int *srctype) {

    if (i < 0 || i >= MAXSHADERS) {
        return;
    }
    Shader* shader = shaders[i];

    if (filename != NULL) {
        SDL_GL_MakeCurrent(vid.window, vid.gl_context);
        LOG_info("loading shader \n");

		char filepath[512];
		snprintf(filepath, sizeof(filepath), SHADERS_FOLDER "/glsl/%s",filename);
		const char *shaderSource  = load_shader_source(filepath);
		loadShaderPragmas(shader,shaderSource);

		GLuint vertex_shader1 = load_shader_from_file(GL_VERTEX_SHADER, filename,SHADERS_FOLDER "/glsl");
		GLuint fragment_shader1 = load_shader_from_file(GL_FRAGMENT_SHADER, filename,SHADERS_FOLDER "/glsl");

        // Link the shader program
		if (shader->shader_p != 0) {
			LOG_info("Deleting previous shader %i\n",shader->shader_p);
			glDeleteProgram(shader->shader_p);
		}
        shader->shader_p = link_program(vertex_shader1, fragment_shader1, filename, filepath);

		shader->u_FrameDirection = glGetUniformLocation( shader->shader_p, "FrameDirection");
		shader->u_FrameCount = glGetUniformLocation( shader->shader_p, "FrameCount");
		shader->u_OutputSize = glGetUniformLocation( shader->shader_p, "OutputSize");
		shader->u_TextureSize = glGetUniformLocation( shader->shader_p, "TextureSize");
		shader->u_InputSize = glGetUniformLocation( shader->shader_p, "InputSize");
		shader->OrigInputSize = glGetUniformLocation( shader->shader_p, "OrigInputSize");
		shader->texLocation = glGetUniformLocation(shader->shader_p, "Texture");
		shader->texelSizeLocation = glGetUniformLocation(shader->shader_p, "texelSize");
		for (int i = 0; i < shader->num_pragmas; ++i) {
			shader->pragmas[i].uniformLocation = glGetUniformLocation(shader->shader_p, shader->pragmas[i].name);
			shader->pragmas[i].value = shader->pragmas[i].def;

			LOG_info("Param: %s = %f (min: %f, max: %f, step: %f)\n",
				shader->pragmas[i].name,
				shader->pragmas[i].def,
				shader->pragmas[i].min,
				shader->pragmas[i].max,
				shader->pragmas[i].step);
		}

        if (shader->shader_p == 0) {
            LOG_info("Shader linking failed for %s\n", filename);
        }

        GLint success = 0;
        glGetProgramiv(shader->shader_p, GL_LINK_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetProgramInfoLog(shader->shader_p, 512, NULL, infoLog);
            LOG_info("Shader Program Linking Failed: %s\n", infoLog);
        } else {
			LOG_info("Shader Program Linking Success %s shader ID is %i\n", filename,shader->shader_p);
		}
		shader->filename = strdup(filename);
    }
    if (scale != NULL) {
        shader->scale = *scale +1;
		reloadShaderTextures = 1;
    }
    if (scaletype != NULL) {
        shader->scaletype = *scaletype;
    }
    if (srctype != NULL) {
        shader->srctype = *srctype;
    }
    if (filter != NULL) {
        shader->filter = (*filter == 1) ? GL_LINEAR : GL_NEAREST;
		reloadShaderTextures = 1;
    }
	shader->updated = 1;

}


void PLAT_setShaders(int nr) {
	LOG_info("set nr of shaders to %i\n",nr);
	nrofshaders = nr;
	reloadShaderTextures = 1;
}

static void clearVideo(void) {
	for (int i=0; i<3; i++) {
		SDL_RenderClear(vid.renderer);
		SDL_FillRect(vid.screen, NULL, vid.clear_color);
		SDL_RenderCopy(vid.renderer, vid.stream_layer1, NULL, NULL);
		SDL_RenderPresent(vid.renderer);
	}
}

void PLAT_quitVideo(void) {
	clearVideo();

	// Make sure the GL context is current before tearing down textures/renderer
	SDL_GL_MakeCurrent(vid.window, vid.gl_context);

	// Destroy textures while renderer is valid
	if (vid.target) SDL_DestroyTexture(vid.target);
	if (vid.effect) SDL_DestroyTexture(vid.effect);
	if (vid.overlay) SDL_DestroyTexture(vid.overlay);
	if (vid.target_layer3) SDL_DestroyTexture(vid.target_layer3);
	if (vid.target_layer1) SDL_DestroyTexture(vid.target_layer1);
	if (vid.target_layer2) SDL_DestroyTexture(vid.target_layer2);
	if (vid.target_layer4) SDL_DestroyTexture(vid.target_layer4);
	if (vid.target_layer5) SDL_DestroyTexture(vid.target_layer5);
	if (vid.stream_layer1) SDL_DestroyTexture(vid.stream_layer1);

	// Ensure all pending GL operations complete before destroying renderer/context
	SDL_RenderFlush(vid.renderer);
	glFinish();

	// Destroy renderer BEFORE GL context - this stops rendering threads
	SDL_DestroyRenderer(vid.renderer);
	vid.renderer = NULL;

	// Drop current context and delete
	SDL_GL_MakeCurrent(vid.window, vid.gl_context);
	SDL_GL_MakeCurrent(NULL, NULL);
	SDL_GL_DeleteContext(vid.gl_context);
	vid.gl_context = NULL;
	SDL_FreeSurface(vid.screen);

	// Zero fb0 before dropping the EGL surface: DestroyWindow causes display to fall back to
	// fb0; clearing it here ensures that fallback is black rather than stale pak content.
	system("cat /dev/zero > /dev/fb0 2>/dev/null");

	// Cleanup and shutdown
	SDL_DestroyWindow(vid.window);
	if (overlay_path) free(overlay_path);

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void PLAT_clearVideo(SDL_Surface* screen) {
	// SDL_FillRect(screen, NULL, 0); // TODO: revisit
	SDL_FillRect(screen, NULL, SDL_transparentBlack);
}
void PLAT_clearAll(void) {
	// ok honestely mixing SDL and OpenGL is really bad, but hey it works just got to sometimes clear gpu stuff and pull context back to SDL
	// so yeah clear all layers and pull a flip() to make it switch back to SDL before clearing
	PLAT_clearLayers(0);
	PLAT_flip(vid.screen,0);
	PLAT_clearLayers(0);
	PLAT_flip(vid.screen,0);

	// then do normal SDL clearing stuff
	PLAT_clearVideo(vid.screen);
	SDL_SetRenderDrawColor(vid.renderer, 0, 0, 0, 0);
	SDL_RenderClear(vid.renderer);
}

void PLAT_setVsync(int vsync) {
	// No effect on Ge8300
	//int interval = 1;
	//if (vsync == VSYNC_OFF) interval = 0;
	//else if (vsync == VSYNC_LENIENT) interval = -1; // Adaptive, fallback to 1 usually happens internally if not supported
	//
	//// Try to set swap interval
	//if (SDL_GL_SetSwapInterval(interval) < 0) {
	//	// If -1 (adaptive) failed, try 1 (strict)
	//	if (interval == -1) {
	//		LOG_info("Adaptive VSync not supported, falling back to Strict\n");
	//		SDL_GL_SetSwapInterval(1);
	//	} else {
	//		LOG_error("Failed to set swap interval: %s\n", SDL_GetError());
	//	}
	//} else {
	//	LOG_info("VSync set to %d (requested %d)\n", interval, vsync);
	//}
}

static int hard_scale = 4; // TODO: base src size, eg. 160x144 can be 4


static void resizeVideo(int w, int h, int p) {
	if (w==vid.width && h==vid.height && p==vid.pitch) return;

	// TODO: minarch disables crisp (and nn upscale before linear downscale) when native, is this true?

	if (w>=device_width && h>=device_height) hard_scale = 1;
	// else if (h>=160) hard_scale = 2; // limits gba and up to 2x (seems sufficient for 640x480)
	else hard_scale = 4;

	// LOG_info("resizeVideo(%i,%i,%i) hard_scale: %i crisp: %i\n",w,h,p, hard_scale,vid.sharpness==SHARPNESS_CRISP);

	SDL_DestroyTexture(vid.stream_layer1);
	if (vid.target) SDL_DestroyTexture(vid.target);

	// SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, vid.sharpness==SHARPNESS_SOFT?"1":"0", SDL_HINT_OVERRIDE);
	vid.stream_layer1 = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w,h);
	SDL_SetTextureBlendMode(vid.stream_layer1, getPremultipliedBlendMode());

	if (vid.sharpness==SHARPNESS_CRISP) {
		// SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, "1", SDL_HINT_OVERRIDE);
		vid.target = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w * hard_scale,h * hard_scale);
	}
	else {
		vid.target = NULL;
	}


	vid.width	= w;
	vid.height	= h;
	vid.pitch	= p;

	reloadShaderTextures = 1;
}

SDL_Surface* PLAT_resizeVideo(int w, int h, int p) {
	resizeVideo(w,h,p);
	return vid.screen;
}

void PLAT_setSharpness(int sharpness) {
	if(sharpness==1) {
		finalScaleFilter=GL_LINEAR;
	}
	else {
		finalScaleFilter = GL_NEAREST;
	}
	reloadShaderTextures = 1;
}

static struct FX_Context {
	int scale;
	int type;
	int color;
	int next_scale;
	int next_type;
	int next_color;
	int live_type;
} effect = {
	.scale = 1,
	.next_scale = 1,
	.type = EFFECT_NONE,
	.next_type = EFFECT_NONE,
	.live_type = EFFECT_NONE,
	.color = 0,
	.next_color = 0,
};
static void rgb565_to_rgb888(uint32_t rgb565, uint8_t *r, uint8_t *g, uint8_t *b) {
    // Extract the red component (5 bits)
    uint8_t red = (rgb565 >> 11) & 0x1F;
    // Extract the green component (6 bits)
    uint8_t green = (rgb565 >> 5) & 0x3F;
    // Extract the blue component (5 bits)
    uint8_t blue = rgb565 & 0x1F;

    // Scale the values to 8-bit range
    *r = (red << 3) | (red >> 2);
    *g = (green << 2) | (green >> 4);
    *b = (blue << 3) | (blue >> 2);
}
static char* effect_path;
static int effectUpdated = 0;
static pthread_mutex_t video_prep_mutex = PTHREAD_MUTEX_INITIALIZER;

static void updateEffect(void) {
	// Read effect state with mutex protection
	pthread_mutex_lock(&video_prep_mutex);
	int next_scale = effect.next_scale;
	int next_type = effect.next_type;
	int next_color = effect.next_color;
	int curr_scale = effect.scale;
	int curr_type = effect.type;
	int curr_color = effect.color;
	pthread_mutex_unlock(&video_prep_mutex);

	if (next_scale==curr_scale && next_type==curr_type && next_color==curr_color) return; // unchanged

	// Update effect state with mutex protection
	pthread_mutex_lock(&video_prep_mutex);
	int live_scale = effect.scale;
	int live_color = effect.color;
	effect.scale = effect.next_scale;
	effect.type = effect.next_type;
	effect.color = effect.next_color;
	int effect_type = effect.type;
	int effect_scale = effect.scale;
	int effect_color = effect.color;
	int live_type = effect.live_type;
	pthread_mutex_unlock(&video_prep_mutex);

	if (effect_type==EFFECT_NONE) return; // disabled
	if (effect_type==live_type && effect_scale==live_scale && effect_color==live_color) return; // already loaded

	int opacity = 128; // 1 - 1/2 = 50%
	if (effect_type==EFFECT_LINE) {
		if (effect_scale<3) {
			effect_path = RES_PATH "/line-2.png";
		}
		else if (effect_scale<4) {
			effect_path = RES_PATH "/line-3.png";
		}
		else if (effect_scale<5) {
			effect_path = RES_PATH "/line-4.png";
		}
		else if (effect_scale<6) {
			effect_path = RES_PATH "/line-5.png";
		}
		else if (effect_scale<8) {
			effect_path = RES_PATH "/line-6.png";
		}
		else {
			effect_path = RES_PATH "/line-8.png";
		}
	}
	else if (effect_type==EFFECT_GRID) {
		if (effect_scale<3) {
			effect_path = RES_PATH "/grid-2.png";
			opacity = 64; // 1 - 3/4 = 25%
		}
		else if (effect_scale<4) {
			effect_path = RES_PATH "/grid-3.png";
			opacity = 112; // 1 - 5/9 = ~44%
		}
		else if (effect_scale<5) {
			effect_path = RES_PATH "/grid-4.png";
			opacity = 144; // 1 - 7/16 = ~56%
		}
		else if (effect_scale<6) {
			effect_path = RES_PATH "/grid-5.png";
			opacity = 160; // 1 - 9/25 = ~64%
			// opacity = 96; // TODO: tmp, for white grid
		}
		else if (effect_scale<8) {
			effect_path = RES_PATH "/grid-6.png";
			opacity = 112; // 1 - 5/9 = ~44%
		}
		else if (effect_scale<11) {
			effect_path = RES_PATH "/grid-8.png";
			opacity = 144; // 1 - 7/16 = ~56%
		}
		else {
			effect_path = RES_PATH "/grid-11.png";
			opacity = 136; // 1 - 57/121 = ~52%
		}
	}
	effectUpdated = 1;

}
int screenx = 0;
int screeny = 0;
void PLAT_setOffsetX(int x) {
    if (x < 0 || x > 128) return;
    screenx = x - 64;
	LOG_info("screenx: %i %i\n",screenx,x);
}
void PLAT_setOffsetY(int y) {
    if (y < 0 || y > 128) return;
    screeny = y - 64;
	LOG_info("screeny: %i %i\n",screeny,y);
}
static int overlayUpdated=0;
void PLAT_setOverlay(const char* filename, const char* tag) {
    if (vid.overlay) {
        SDL_DestroyTexture(vid.overlay);
        vid.overlay = NULL;
    }
	if (overlay_path) {
		free(overlay_path);
		overlay_path = NULL;
	}

	pthread_mutex_lock(&video_prep_mutex);
	overlayUpdated=1;
	pthread_mutex_unlock(&video_prep_mutex);

    if (!filename || strcmp(filename, "") == 0 || strcmp(filename, "None") == 0) {
		overlay_path = strdup("");
        LOG_info("Skipping overlay update.\n");
        return;
    }

    size_t path_len = strlen(OVERLAYS_FOLDER) + strlen(tag) + strlen(filename) + 4; // +3 for slashes and null-terminator
    overlay_path = malloc(path_len);

    if (!overlay_path) {
        perror("malloc failed");
        return;
    }

    snprintf(overlay_path, path_len, "%s/%s/%s", OVERLAYS_FOLDER, tag, filename);
    LOG_info("Overlay path set to: %s\n", overlay_path);

}


void applyRoundedCorners(SDL_Surface* surface, SDL_Rect* rect, int radius) {
	if (!surface) return;

    Uint32* pixels = (Uint32*)surface->pixels;
    SDL_PixelFormat* fmt = surface->format;
	SDL_Rect target = {0, 0, surface->w, surface->h};
	if (rect)
		target = *rect;

    Uint32 transparent_black = SDL_MapRGBA(fmt, 0, 0, 0, 0);  // Fully transparent black

	const int xBeg = target.x;
	const int xEnd = target.x + target.w;
	const int yBeg = target.y;
	const int yEnd = target.y + target.h;
	for (int y = yBeg; y < yEnd; ++y)
	{
		for (int x = xBeg; x < xEnd; ++x) {
            int dx = (x < xBeg + radius) ? xBeg + radius - x : (x >= xEnd - radius) ? x - (xEnd - radius - 1) : 0;
            int dy = (y < yBeg + radius) ? yBeg + radius - y : (y >= yEnd - radius) ? y - (yEnd - radius - 1) : 0;
            if (dx * dx + dy * dy > radius * radius) {
                pixels[y * target.w + x] = transparent_black;  // Set to fully transparent black
            }
        }
    }
}

void PLAT_clearLayers(int layer) {
	if(layer==0 || layer==1) {
		uint32_t bg = vid.clear_color;
		SDL_SetRenderTarget(vid.renderer, vid.target_layer1);
		SDL_SetRenderDrawColor(vid.renderer, (bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF, 255);
		SDL_RenderClear(vid.renderer);
	}
	SDL_SetRenderDrawColor(vid.renderer, 0, 0, 0, 0);
	if(layer==0 || layer==2) {
		SDL_SetRenderTarget(vid.renderer, vid.target_layer2);
		SDL_RenderClear(vid.renderer);
	}
	if(layer==0 || layer==3) {
		SDL_SetRenderTarget(vid.renderer, vid.target_layer3);
		SDL_RenderClear(vid.renderer);
	}
	if(layer==0 || layer==4) {
		SDL_SetRenderTarget(vid.renderer, vid.target_layer4);
		SDL_RenderClear(vid.renderer);
	}
	if(layer==0 || layer==5) {
		SDL_SetRenderTarget(vid.renderer, vid.target_layer5);
		SDL_RenderClear(vid.renderer);
	}

	SDL_SetRenderTarget(vid.renderer, NULL);
}

void PLAT_drawOnLayer(SDL_Surface *inputSurface, int x, int y, int w, int h, float brightness, bool maintainAspectRatio,int layer) {
    if (!inputSurface || !vid.target_layer1 || !vid.renderer) return;

    SDL_Texture* tempTexture = SDL_CreateTexture(vid.renderer,
                                                 SDL_PIXELFORMAT_ARGB8888,
                                                 SDL_TEXTUREACCESS_TARGET,
                                                 inputSurface->w, inputSurface->h);

    if (!tempTexture) {
        LOG_error("Failed to create temporary texture: %s\n", SDL_GetError());
        return;
    }

    SDL_UpdateTexture(tempTexture, NULL, inputSurface->pixels, inputSurface->pitch);
    switch (layer)
	{
	case 1:
		SDL_SetRenderTarget(vid.renderer, vid.target_layer1);
		break;
	case 2:
		SDL_SetRenderTarget(vid.renderer, vid.target_layer2);
		break;
	case 3:
		SDL_SetRenderTarget(vid.renderer, vid.target_layer3);
		break;
	case 4:
		SDL_SetRenderTarget(vid.renderer, vid.target_layer4);
		break;
	case 5:
		SDL_SetRenderTarget(vid.renderer, vid.target_layer5);
		break;
	default:
		SDL_SetRenderTarget(vid.renderer, vid.target_layer1);
		break;
	}

    // Adjust brightness
    Uint8 r = 255, g = 255, b = 255;
    if (brightness < 1.0f) {
        r = g = b = (Uint8)(255 * brightness);
    } else if (brightness > 1.0f) {
        r = g = b = 255;
    }

    SDL_SetTextureColorMod(tempTexture, r, g, b);

    // Aspect ratio handling
    SDL_Rect srcRect = { 0, 0, inputSurface->w, inputSurface->h };
    SDL_Rect dstRect = { x, y, w, h };

    if (maintainAspectRatio) {
        float aspectRatio = (float)inputSurface->w / (float)inputSurface->h;

        if (w / (float)h > aspectRatio) {
            dstRect.w = (int)(h * aspectRatio);
        } else {
            dstRect.h = (int)(w / aspectRatio);
        }
    }

    SDL_RenderCopy(vid.renderer, tempTexture, &srcRect, &dstRect);
    SDL_SetRenderTarget(vid.renderer, NULL);
    SDL_DestroyTexture(tempTexture);
}


void PLAT_animateSurface(
	SDL_Surface *inputSurface,
	int x, int y,
	int target_x, int target_y,
	int w, int h,
	int duration_ms,
	int start_opacity,
	int target_opacity,
	int layer
) {
	if (!inputSurface || !vid.target_layer2 || !vid.renderer) return;

	SDL_Texture* tempTexture = SDL_CreateTexture(vid.renderer,
		SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_TARGET,
		inputSurface->w, inputSurface->h);

	if (!tempTexture) {
		LOG_error("Failed to create temporary texture: %s\n", SDL_GetError());
		return;
	}

	SDL_UpdateTexture(tempTexture, NULL, inputSurface->pixels, inputSurface->pitch);
	SDL_SetTextureBlendMode(tempTexture, SDL_BLENDMODE_BLEND);  // Enable blending for opacity

	const int fps = 60;
	const int frame_delay = 1000 / fps;
	const int total_frames = duration_ms / frame_delay;

	for (int frame = 0; frame <= total_frames; ++frame) {

		float t = (float)frame / total_frames;

		int current_x = x + (int)((target_x - x) * t);
		int current_y = y + (int)((target_y - y) * t);

		// Interpolate opacity
		int current_opacity = start_opacity + (int)((target_opacity - start_opacity) * t);
		if (current_opacity < 0) current_opacity = 0;
		if (current_opacity > 255) current_opacity = 255;

		SDL_SetTextureAlphaMod(tempTexture, current_opacity);

		if (layer == 0)
			SDL_SetRenderTarget(vid.renderer, vid.target_layer2);
		else
			SDL_SetRenderTarget(vid.renderer, vid.target_layer4);

		SDL_SetRenderDrawColor(vid.renderer, 0, 0, 0, 0);
		SDL_RenderClear(vid.renderer);

		SDL_Rect srcRect = { 0, 0, inputSurface->w, inputSurface->h };
		SDL_Rect dstRect = { current_x, current_y, w, h };
		SDL_RenderCopy(vid.renderer, tempTexture, &srcRect, &dstRect);

		SDL_SetRenderTarget(vid.renderer, NULL);
		PLAT_GPU_Flip();
	}

	SDL_DestroyTexture(tempTexture);
}

int PLAT_textShouldScroll(TTF_Font* font, const char* in_name,int max_width, SDL_mutex* fontMutex) {
    int text_width = 0;
	if (fontMutex) SDL_LockMutex(fontMutex);
	TTF_SizeUTF8(font, in_name, &text_width, NULL);
	if (fontMutex) SDL_UnlockMutex(fontMutex);

	if (text_width <= max_width) {
		return 0;
	} else {
		return 1;
	}
}
static int text_offset = 0;
void PLAT_resetScrollText() {
	text_offset = 0;
}
void PLAT_scrollTextTexture(
    TTF_Font* font,
    const char* in_name,
    int x, int y,      // Position on target layer
    int w, int h,      // Clipping width and height
    SDL_Color color,
    float transparency,
    SDL_mutex* fontMutex  // Mutex for thread-safe font access (can be NULL)
) {
    static int frame_counter = 0;
	int padding = 30;

    if (transparency < 0.0f) transparency = 0.0f;
    if (transparency > 1.0f) transparency = 1.0f;
    color.a = (Uint8)(transparency * 255);

    // Render the original text with mutex protection for thread safety
    if (fontMutex) SDL_LockMutex(fontMutex);
    SDL_Surface* singleSur = TTF_RenderUTF8_Blended(font, in_name, color);
    if (fontMutex) SDL_UnlockMutex(fontMutex);
    if (!singleSur) return;

    int single_width = singleSur->w;
    int single_height = singleSur->h;

    // Create a surface to hold two copies side by side with padding
    SDL_Surface* text_surface = SDL_CreateRGBSurfaceWithFormat(0,
        single_width * 2 + padding, single_height, 32, SDL_PIXELFORMAT_ARGB8888);

    SDL_FillRect(text_surface, NULL, THEME_COLOR1);
    SDL_BlitSurface(singleSur, NULL, text_surface, NULL);

    SDL_Rect second = { single_width + padding, 0, single_width, single_height };
    SDL_BlitSurface(singleSur, NULL, text_surface, &second);
    SDL_FreeSurface(singleSur);

    SDL_Texture* full_text_texture = SDL_CreateTextureFromSurface(vid.renderer, text_surface);
    int full_text_width = text_surface->w;
    SDL_FreeSurface(text_surface);

    if (!full_text_texture) return;

    SDL_SetTextureBlendMode(full_text_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(full_text_texture, color.a);

    SDL_SetRenderTarget(vid.renderer, vid.target_layer4);

    SDL_Rect src_rect = { text_offset, 0, w, single_height };
    SDL_Rect dst_rect = { x, y, w, single_height };

    SDL_RenderCopy(vid.renderer, full_text_texture, &src_rect, &dst_rect);

    SDL_SetRenderTarget(vid.renderer, NULL);
    SDL_DestroyTexture(full_text_texture);

    // Scroll only if text is wider than clip width
    if (single_width > w) {
        frame_counter++;
        if (frame_counter >= 0) {
            text_offset += 2;
            if (text_offset >= single_width + padding) {
                text_offset = 0;
            }
            frame_counter = 0;
        }
    } else {
        text_offset = 0;
    }

    PLAT_GPU_Flip();
}


// super fast without update_texture to draw screen
void PLAT_GPU_Flip() {
	SDL_RenderClear(vid.renderer);
	SDL_RenderCopy(vid.renderer, vid.target_layer1, NULL, NULL);
	SDL_RenderCopy(vid.renderer, vid.target_layer2, NULL, NULL);
	SDL_RenderCopy(vid.renderer, vid.stream_layer1, NULL, NULL);
	SDL_RenderCopy(vid.renderer, vid.target_layer3, NULL, NULL);
	SDL_RenderCopy(vid.renderer, vid.target_layer4, NULL, NULL);
	SDL_RenderCopy(vid.renderer, vid.target_layer5, NULL, NULL);
	SDL_RenderPresent(vid.renderer);
}

void PLAT_animateSurfaceOpacity(
	SDL_Surface *inputSurface,
	int x, int y, int w, int h,
	int start_opacity, int target_opacity,
	int duration_ms,
	int layer
) {
	if (!inputSurface) return;

	SDL_Texture* tempTexture = SDL_CreateTexture(vid.renderer,
		SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_TARGET,
		inputSurface->w, inputSurface->h);

	if (!tempTexture) {
		LOG_error("Failed to create temporary texture: %s\n", SDL_GetError());
		return;
	}

	SDL_UpdateTexture(tempTexture, NULL, inputSurface->pixels, inputSurface->pitch);
	SDL_SetTextureBlendMode(tempTexture, SDL_BLENDMODE_BLEND);

	const int fps = 60;
	const int frame_delay = 1000 / fps;
	const int total_frames = duration_ms / frame_delay;

	SDL_Texture* target_layer = (layer == 0) ? vid.target_layer2 : vid.target_layer4;
	if (!target_layer) {
		SDL_DestroyTexture(tempTexture);
		return;
	}

	for (int frame = 0; frame <= total_frames; ++frame) {

		float t = (float)frame / total_frames;
		int current_opacity = start_opacity + (int)((target_opacity - start_opacity) * t);
		if (current_opacity < 0) current_opacity = 0;
		if (current_opacity > 255) current_opacity = 255;

		SDL_SetTextureAlphaMod(tempTexture, current_opacity);
		SDL_SetRenderTarget(vid.renderer, target_layer);
		SDL_SetRenderDrawColor(vid.renderer, 0, 0, 0, 0);
		SDL_RenderClear(vid.renderer);

		SDL_Rect dstRect = { x, y, w, h };
		SDL_RenderCopy(vid.renderer, tempTexture, NULL, &dstRect);

		SDL_SetRenderTarget(vid.renderer, NULL);
		// blit to 0 for normal draw
		vid.blit = 0;
		PLAT_flip(vid.screen,0);

	}

	SDL_DestroyTexture(tempTexture);
}

SDL_Surface* PLAT_captureRendererToSurface() {
	if (!vid.renderer) return NULL;

	int width, height;
	SDL_GetRendererOutputSize(vid.renderer, &width, &height);

	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!surface) {
		LOG_error("Failed to create surface: %s\n", SDL_GetError());
		return NULL;
	}

	Uint32 black = SDL_MapRGBA(surface->format, 0, 0, 0, 255);
	SDL_FillRect(surface, NULL, black);

	if (SDL_RenderReadPixels(vid.renderer, NULL, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) != 0) {
		LOG_error("Failed to read pixels from renderer: %s\n", SDL_GetError());
		SDL_FreeSurface(surface);
		return NULL;
	}

	// remove transparancy
	Uint32* pixels = (Uint32*)surface->pixels;
	int total_pixels = (surface->pitch / 4) * surface->h;

	for (int i = 0; i < total_pixels; i++) {
		Uint8 r, g, b, a;
		SDL_GetRGBA(pixels[i], surface->format, &r, &g, &b, &a);
		pixels[i] = SDL_MapRGBA(surface->format, r, g, b, 255);
	}

	SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
	return surface;
}

static float anim_ease(float t, int easing, float intensity) {
	switch (easing) {
		case ANIM_EASE_OUT:
			return 1.0f - powf(1.0f - t, intensity);
		case ANIM_EASE_IN:
			return powf(t, intensity);
		case ANIM_EASE_IN_OUT:
			return t < 0.5f
				? 0.5f * powf(2.0f * t, intensity)
				: 1.0f - 0.5f * powf(2.0f * (1.0f - t), intensity);
		default:
			return t;
	}
}

void PLAT_animateAndFadeSurface(
	SDL_Surface *inputSurface,
	int x, int y, int target_x, int target_y, int w, int h, int duration_ms,
	SDL_Surface *fadeSurface,
	int fade_x, int fade_y, int fade_target_x, int fade_target_y, int fade_w, int fade_h,
	int start_opacity, int target_opacity, int layer,
	int input_easing, int fade_easing, int intensity
) {
	if (!inputSurface || !vid.renderer) return;

	SDL_Texture* moveTexture = SDL_CreateTexture(vid.renderer,
		SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_TARGET,
		inputSurface->w, inputSurface->h);

	if (!moveTexture) {
		LOG_error("Failed to create move texture: %s\n", SDL_GetError());
		return;
	}

	SDL_UpdateTexture(moveTexture, NULL, inputSurface->pixels, inputSurface->pitch);

	SDL_Texture* fadeTexture = NULL;
	if (fadeSurface) {
		fadeTexture = SDL_CreateTextureFromSurface(vid.renderer, fadeSurface);
		if (!fadeTexture) {
			LOG_error("Failed to create fade texture: %s\n", SDL_GetError());
			SDL_DestroyTexture(moveTexture);
			return;
		}
		SDL_SetTextureBlendMode(fadeTexture, SDL_BLENDMODE_BLEND);
	}

	const int fps = 60;
	const int frame_delay = 1000 / fps;
	const int total_frames = duration_ms / frame_delay;

	Uint32 start_time = SDL_GetTicks();

	for (int frame = 0; frame <= total_frames; ++frame) {

		float t = (float)frame / total_frames;
		float t_input = anim_ease(t, input_easing, (float)intensity);
		float t_fade  = anim_ease(t, fade_easing, (float)intensity);

		int current_x = x + (int)((target_x - x) * t_input);
		int current_y = y + (int)((target_y - y) * t_input);

		int current_opacity = start_opacity + (int)((target_opacity - start_opacity) * t);
		if (current_opacity < 0) current_opacity = 0;
		if (current_opacity > 255) current_opacity = 255;

		switch (layer)
		{
		case 1:
			SDL_SetRenderTarget(vid.renderer, vid.target_layer1);
			break;
		case 2:
			SDL_SetRenderTarget(vid.renderer, vid.target_layer2);
			break;
		case 3:
			SDL_SetRenderTarget(vid.renderer, vid.target_layer3);
			break;
		case 4:
			SDL_SetRenderTarget(vid.renderer, vid.target_layer4);
			break;
		case 5:
			SDL_SetRenderTarget(vid.renderer, vid.target_layer5);
			break;
		default:
			SDL_SetRenderTarget(vid.renderer, vid.target_layer1);
			break;
		}
		SDL_SetRenderDrawColor(vid.renderer, 0, 0, 0, 0);
		SDL_RenderClear(vid.renderer);

		SDL_Rect moveSrcRect = { 0, 0, inputSurface->w, inputSurface->h };
		SDL_Rect moveDstRect = { current_x, current_y, w, h };
		SDL_RenderCopy(vid.renderer, moveTexture, &moveSrcRect, &moveDstRect);

		if (fadeTexture) {
			SDL_SetTextureAlphaMod(fadeTexture, current_opacity);
			int fade_current_x = fade_x + (int)((fade_target_x - fade_x) * t_fade);
			int fade_current_y = fade_y + (int)((fade_target_y - fade_y) * t_fade);
			SDL_Rect fadeDstRect = { fade_current_x, fade_current_y, fade_w, fade_h };
			SDL_RenderCopy(vid.renderer, fadeTexture, NULL, &fadeDstRect);
		}

		SDL_SetRenderTarget(vid.renderer, NULL);
		PLAT_GPU_Flip();

	}

	SDL_DestroyTexture(moveTexture);
	if (fadeTexture) SDL_DestroyTexture(fadeTexture);
}

void PLAT_setEffect(int next_type) {
	pthread_mutex_lock(&video_prep_mutex);
	effect.next_type = next_type;
	pthread_mutex_unlock(&video_prep_mutex);
}
void PLAT_setEffectColor(int next_color) {
	pthread_mutex_lock(&video_prep_mutex);
	effect.next_color = next_color;
	pthread_mutex_unlock(&video_prep_mutex);
}
void PLAT_vsync(int remaining) {
	if (remaining>0) SDL_Delay(remaining);
}


scaler_t PLAT_getScaler(GFX_Renderer* renderer) {
	// LOG_info("getScaler for scale: %i\n", renderer->scale);
	pthread_mutex_lock(&video_prep_mutex);
	effect.next_scale = renderer->scale;
	pthread_mutex_unlock(&video_prep_mutex);
	return scale1x1_c16;
}

void setRectToAspectRatio(SDL_Rect* dst_rect) {
    int x = vid.blit->src_x;
    int y = vid.blit->src_y;
    int w = vid.blit->src_w;
    int h = vid.blit->src_h;

    if (vid.blit->aspect == 0) {
        w = vid.blit->src_w * vid.blit->scale;
        h = vid.blit->src_h * vid.blit->scale;
        dst_rect->x = (device_width - w) / 2 + screenx;
        dst_rect->y = (device_height - h) / 2 + screeny;
        dst_rect->w = w;
        dst_rect->h = h;
    } else if (vid.blit->aspect > 0) {
        h = device_height;
        w = h * vid.blit->aspect;
        if (w > device_width) {
            w = device_width;
            h = w / vid.blit->aspect;
        }
        dst_rect->x = (device_width - w) / 2 + screenx;
        dst_rect->y = (device_height - h) / 2 + screeny;
        dst_rect->w = w;
        dst_rect->h = h;
    } else {
        dst_rect->x = screenx;
        dst_rect->y = screeny;
        dst_rect->w = device_width;
        dst_rect->h = device_height;
    }
}

void PLAT_blitRenderer(GFX_Renderer* renderer) {
	vid.blit = renderer;
	SDL_RenderClear(vid.renderer);
	resizeVideo(vid.blit->true_w,vid.blit->true_h,vid.blit->src_p);
}

void PLAT_clearShaders() {
	// this funciton was empty so am abusing it for now for this, later need to make a seperate function for it
	// set blit to 0 maybe should be seperate function later
	vid.blit = NULL;
}

void PLAT_flipHidden() {
	SDL_RenderClear(vid.renderer);
	resizeVideo(device_width, device_height, FIXED_PITCH); // !!!???
	SDL_UpdateTexture(vid.stream_layer1, NULL, vid.screen->pixels, vid.screen->pitch);
	SDL_RenderCopy(vid.renderer, vid.target_layer1, NULL, NULL);
	SDL_RenderCopy(vid.renderer, vid.target_layer2, NULL, NULL);
	SDL_RenderCopy(vid.renderer, vid.stream_layer1, NULL, NULL);
	SDL_RenderCopy(vid.renderer, vid.target_layer3, NULL, NULL);
	SDL_RenderCopy(vid.renderer, vid.target_layer4, NULL, NULL);
	SDL_RenderCopy(vid.renderer, vid.target_layer5, NULL, NULL);
	//  SDL_RenderPresent(vid.renderer); // no present want to flip  hidden
}


void PLAT_flip(SDL_Surface* IGNORED, int ignored) {
	// dont think we need this here tbh
	// SDL_RenderClear(vid.renderer);
	if (!vid.blit) {
        resizeVideo(device_width, device_height, FIXED_PITCH); // !!!???
        SDL_UpdateTexture(vid.stream_layer1, NULL, vid.screen->pixels, vid.screen->pitch);
		SDL_RenderCopy(vid.renderer, vid.target_layer1, NULL, NULL);
		SDL_RenderCopy(vid.renderer, vid.target_layer2, NULL, NULL);
		SDL_RenderCopy(vid.renderer, vid.stream_layer1, NULL, NULL);
		SDL_RenderCopy(vid.renderer, vid.target_layer3, NULL, NULL);
		SDL_RenderCopy(vid.renderer, vid.target_layer4, NULL, NULL);
		SDL_RenderCopy(vid.renderer, vid.target_layer5, NULL, NULL);
		SDL_RenderPresent(vid.renderer);
        return;
    }

    // Safety check: ensure texture dimensions match blit buffer dimensions
    if (vid.width != vid.blit->true_w || vid.height != vid.blit->true_h) {
        // Texture size doesn't match buffer, clear blit and use screen buffer instead
        vid.blit = NULL;
        resizeVideo(device_width, device_height, FIXED_PITCH);
        SDL_UpdateTexture(vid.stream_layer1, NULL, vid.screen->pixels, vid.screen->pitch);
		SDL_RenderCopy(vid.renderer, vid.target_layer1, NULL, NULL);
		SDL_RenderCopy(vid.renderer, vid.target_layer2, NULL, NULL);
		SDL_RenderCopy(vid.renderer, vid.stream_layer1, NULL, NULL);
		SDL_RenderCopy(vid.renderer, vid.target_layer3, NULL, NULL);
		SDL_RenderCopy(vid.renderer, vid.target_layer4, NULL, NULL);
		SDL_RenderCopy(vid.renderer, vid.target_layer5, NULL, NULL);
		SDL_RenderPresent(vid.renderer);
        return;
    }

    SDL_UpdateTexture(vid.stream_layer1, NULL, vid.blit->src, vid.blit->src_p);

    SDL_Texture* target = vid.stream_layer1;
    int x = vid.blit->src_x;
    int y = vid.blit->src_y;
    int w = vid.blit->src_w;
    int h = vid.blit->src_h;
    if (vid.sharpness == SHARPNESS_CRISP) {

        SDL_SetRenderTarget(vid.renderer, vid.target);
        SDL_RenderCopy(vid.renderer, vid.stream_layer1, NULL, NULL);
        SDL_SetRenderTarget(vid.renderer, NULL);
        x *= hard_scale;
        y *= hard_scale;
        w *= hard_scale;
        h *= hard_scale;
        target = vid.target;
    }

    SDL_Rect* src_rect = &(SDL_Rect){x, y, w, h};
    SDL_Rect* dst_rect = &(SDL_Rect){0, 0, device_width, device_height};

    setRectToAspectRatio(dst_rect);

    SDL_RenderCopy(vid.renderer, target, src_rect, dst_rect);
    SDL_RenderPresent(vid.renderer);

    vid.blit = NULL;
}

static int frame_count = 0;
void runShaderPass(GLuint src_texture, GLuint shader_program, GLuint* target_texture,
                   int x, int y, int dst_width, int dst_height, Shader* shader, int alpha, int filter) {

	static GLuint static_VAO = 0, static_VBO = 0;
	static GLuint last_program = 0;
	static GLfloat last_texelSize[2] = {-1.0f, -1.0f};
	static GLfloat texelSize[2] = {-1.0f, -1.0f};
	static GLuint fbo = 0;
	static GLuint last_bound_texture = 0;
	static GLint max_tex_size = 0;
	static int logged_bad_size = 0;
	GLenum pre_err;

	while ((pre_err = glGetError()) != GL_NO_ERROR) {
		(void)pre_err;
	}

	if (max_tex_size == 0) {
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_size);
		if (max_tex_size <= 0) {
			max_tex_size = 2048;
		}
	}

	if (dst_width <= 0 || dst_height <= 0 || dst_width > max_tex_size || dst_height > max_tex_size) {
		if (!logged_bad_size) {
			LOG_error("Shader pass invalid target size: %dx%d (max %d)\n", dst_width, dst_height, max_tex_size);
			logged_bad_size = 1;
		}
		return;
	}

	if (shaderResetRequested) {
		// Force rebuild of GL objects and cached state
		if (static_VAO) { glDeleteVertexArrays(1, &static_VAO); static_VAO = 0; }
		if (static_VBO) { glDeleteBuffers(1, &static_VBO); static_VBO = 0; }
		last_program = 0;
		last_texelSize[0] = last_texelSize[1] = -1.0f;
		fbo = 0;
		last_bound_texture = 0;
	}

	texelSize[0] = 1.0f / shader->texw;
	texelSize[1] = 1.0f / shader->texh;


	if (shader_program != last_program)
    	glUseProgram(shader_program);

	if (static_VAO == 0) {
		glGenVertexArrays(1, &static_VAO);
		glGenBuffers(1, &static_VBO);
		glBindVertexArray(static_VAO);
		glBindBuffer(GL_ARRAY_BUFFER, static_VBO);

		float vertices[] = {
			-1.0f,  1.0f, 0.0f, 1.0f,  0.0f, 1.0f, 0.0f, 0.0f,
			-1.0f, -1.0f, 0.0f, 1.0f,  0.0f, 0.0f, 0.0f, 0.0f,
			 1.0f,  1.0f, 0.0f, 1.0f,  1.0f, 1.0f, 0.0f, 0.0f,
			 1.0f, -1.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f, 0.0f
		};

		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	}

	glBindVertexArray(static_VAO);

	if (shader_program != last_program) {
		GLint posAttrib = glGetAttribLocation(shader_program, "VertexCoord");
		if (posAttrib >= 0) {
			glVertexAttribPointer(posAttrib, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
			glEnableVertexAttribArray(posAttrib);
		}
		GLint texAttrib = glGetAttribLocation(shader_program, "TexCoord");
		if (texAttrib >= 0) {
			glVertexAttribPointer(texAttrib,  4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(4 * sizeof(float)));
			glEnableVertexAttribArray(texAttrib);
		}

		if (shader->u_FrameDirection >= 0) glUniform1i(shader->u_FrameDirection, 1);
		if (shader->u_FrameCount >= 0) glUniform1i(shader->u_FrameCount, frame_count);
		if (shader->u_OutputSize >= 0) glUniform2f(shader->u_OutputSize, dst_width, dst_height);
		if (shader->u_TextureSize >= 0) glUniform2f(shader->u_TextureSize, shader->texw, shader->texh);
		if (shader->OrigInputSize >= 0) glUniform2f(shader->OrigInputSize, shader->srcw, shader->srch);
		if (shader->u_InputSize >= 0) glUniform2f(shader->u_InputSize, shader->srcw, shader->srch);
		for (int i = 0; i < shader->num_pragmas; ++i) {
			glUniform1f(shader->pragmas[i].uniformLocation, shader->pragmas[i].value);
		}

		GLint u_MVP = glGetUniformLocation(shader_program, "MVPMatrix");
		if (u_MVP >= 0) {
			static const float identity[16] = {
				 1.0f, 0.0f, 0.0f, 0.0f,
				 0.0f, 1.0f, 0.0f, 0.0f,
				 0.0f, 0.0f, 1.0f, 0.0f,
				 0.0f, 0.0f, 0.0f, 1.0f,
			};
			glUniformMatrix4fv(u_MVP, 1, GL_FALSE, identity);
		}
	}
	if (target_texture) {
		if (*target_texture != 0 && !glIsTexture(*target_texture)) {
			*target_texture = 0;
			shader->updated = 1;
		}
		if (*target_texture==0 || shader->updated || reloadShaderTextures) {

			// if(target_texture) {
			// 	glDeleteTextures(1,target_texture);
			// }
			if(*target_texture==0)
				glGenTextures(1, target_texture);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, *target_texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dst_width, dst_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			shader->updated = 0;
		}
		if (fbo == 0) {
			glGenFramebuffers(1, &fbo);
		}

        // Always bind before attaching to avoid stale state after swaps
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *target_texture, 0);

		GLenum  err = glGetError();
		if (err != GL_NO_ERROR) {
			LOG_error("Framebuffer error: %d\n", err);
			LOG_info("Failed to bind framebuffer with texture %u\n", *target_texture);
		}

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_error("Framebuffer incomplete: 0x%X\n", status);
        }

    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

	if(alpha==1) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glDisable(GL_BLEND);
	}

	if (src_texture != last_bound_texture) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src_texture);
		last_bound_texture = src_texture;
	}
	glViewport(x, y, dst_width, dst_height);

	if (shader->texLocation >= 0) glUniform1i(shader->texLocation, 0);

	if (shader->texelSizeLocation >= 0) {
		glUniform2fv(shader->texelSizeLocation, 1, texelSize);
		last_texelSize[0] = texelSize[0];
		last_texelSize[1] = texelSize[1];
	}
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	last_program = shader_program;
}

typedef struct {
    SDL_Surface* loaded_effect;
    SDL_Surface* loaded_overlay;
    int effect_ready;
    int overlay_ready;
} FramePreparation;

static FramePreparation frame_prep = {0};

int prepareFrameThread(void *data) {
    while (1) {
		updateEffect();

		// Check effectUpdated flag with mutex
		pthread_mutex_lock(&video_prep_mutex);
		int effect_updated = effectUpdated;
		pthread_mutex_unlock(&video_prep_mutex);

        if (effect_updated) {
			LOG_info("effect updated %s\n",effect_path);
			if(effect_path) {
				SDL_Surface* tmp = IMG_Load(effect_path);
				SDL_Surface* converted = NULL;
				if (tmp) {
					converted = SDL_ConvertSurfaceFormat(tmp, SDL_PIXELFORMAT_RGBA32, 0);
					SDL_FreeSurface(tmp);
				}

				pthread_mutex_lock(&video_prep_mutex);
				frame_prep.loaded_effect = converted;
				effectUpdated = 0;
				frame_prep.effect_ready = 1;
				pthread_mutex_unlock(&video_prep_mutex);
			} else {
				pthread_mutex_lock(&video_prep_mutex);
				frame_prep.loaded_effect = 0;
				effectUpdated = 0;
				frame_prep.effect_ready = 1;
				pthread_mutex_unlock(&video_prep_mutex);
			}
        }

		// Check if effect is disabled
		pthread_mutex_lock(&video_prep_mutex);
		int effect_type = effect.type;
		SDL_Surface* loaded_effect = frame_prep.loaded_effect;
		pthread_mutex_unlock(&video_prep_mutex);

		if(effect_type == EFFECT_NONE && loaded_effect != 0) {
			pthread_mutex_lock(&video_prep_mutex);
			frame_prep.loaded_effect = 0;
			frame_prep.effect_ready = 1;
			pthread_mutex_unlock(&video_prep_mutex);
		}

		// Check overlayUpdated flag with mutex
		pthread_mutex_lock(&video_prep_mutex);
		int overlay_updated = overlayUpdated;
		pthread_mutex_unlock(&video_prep_mutex);

        if (overlay_updated) {

			LOG_info("overlay updated\n");
			if(overlay_path) {
				SDL_Surface* tmp = IMG_Load(overlay_path);
				SDL_Surface* converted = NULL;
				if (tmp) {
					converted = SDL_ConvertSurfaceFormat(tmp, SDL_PIXELFORMAT_RGBA32, 0);
					SDL_FreeSurface(tmp);
				}

				pthread_mutex_lock(&video_prep_mutex);
				frame_prep.loaded_overlay = converted;
				frame_prep.overlay_ready = 1;
				overlayUpdated=0;
				pthread_mutex_unlock(&video_prep_mutex);
			} else {
				pthread_mutex_lock(&video_prep_mutex);
				frame_prep.loaded_overlay = 0;
				frame_prep.overlay_ready = 1;
				overlayUpdated=0;
				pthread_mutex_unlock(&video_prep_mutex);
			}
        }

        SDL_Delay(120);
    }
    return 0;
}

static SDL_Thread *prepare_thread = NULL;

void PLAT_GL_Swap() {

	//uint64_t performance_frequency = SDL_GetPerformanceFrequency();
	//uint64_t frame_start = SDL_GetPerformanceCounter();

	if (prepare_thread == NULL) {
        prepare_thread = SDL_CreateThread(prepareFrameThread, "PrepareFrameThread", NULL);

        if (prepare_thread == NULL) {
            LOG_error("Error creating background thread: %s\n", SDL_GetError());
            return;
        }
    }

    static int lastframecount = 0;
    if (reloadShaderTextures) lastframecount = frame_count;
    if (frame_count < lastframecount + 3 || notif.clear_frames > 0) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (notif.clear_frames > 0) notif.clear_frames--;
    }

    SDL_Rect dst_rect = {0, 0, device_width, device_height};
    setRectToAspectRatio(&dst_rect);

    if (!vid.blit->src) {
        return;
    }

	SDL_GL_MakeCurrent(vid.window, vid.gl_context);

	static GLuint effect_tex = 0;
	static int effect_w = 0, effect_h = 0;
	static GLuint overlay_tex = 0;
	static int overlay_w = 0, overlay_h = 0;
	static int overlayload = 0;

	static GLuint src_texture = 0;
	static int src_w_last = 0, src_h_last = 0;
	static int last_w = 0, last_h = 0;

	if (shaderResetRequested) {
		if (src_texture) { glDeleteTextures(1, &src_texture); src_texture = 0; }
		src_w_last = src_h_last = 0;
		last_w = last_h = 0;
		if (effect_tex) {
			glDeleteTextures(1, &effect_tex);
			effect_tex = 0;
			effect_w = effect_h = 0;
			// Force reload by marking as ready again if effect is active
			pthread_mutex_lock(&video_prep_mutex);
			if (effect.type != EFFECT_NONE) {
				frame_prep.effect_ready = 1;
			}
			pthread_mutex_unlock(&video_prep_mutex);
		}
		if (overlay_tex) {
			glDeleteTextures(1, &overlay_tex);
			overlay_tex = 0;
			overlay_w = overlay_h = 0;
			// Force reload if we had an overlay
			pthread_mutex_lock(&video_prep_mutex);
			if (frame_prep.loaded_overlay) {
				frame_prep.overlay_ready = 1;
			}
			pthread_mutex_unlock(&video_prep_mutex);
		}
		reloadShaderTextures = 1;
	}

	// Check if effect needs updating
	pthread_mutex_lock(&video_prep_mutex);
	int effect_ready = frame_prep.effect_ready;
	SDL_Surface* loaded_effect = frame_prep.loaded_effect;
	pthread_mutex_unlock(&video_prep_mutex);

	if (effect_ready) {
		if(loaded_effect) {
			if(!effect_tex) glGenTextures(1, &effect_tex);
			glBindTexture(GL_TEXTURE_2D, effect_tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, loaded_effect->w, loaded_effect->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, loaded_effect->pixels);
			effect_w = loaded_effect->w;
			effect_h = loaded_effect->h;
		} else {
			if (effect_tex) {
				glDeleteTextures(1, &effect_tex);
			}
			effect_tex = 0;
		}
		pthread_mutex_lock(&video_prep_mutex);
        frame_prep.effect_ready = 0;
		pthread_mutex_unlock(&video_prep_mutex);
    }

	// Check if overlay needs updating
	pthread_mutex_lock(&video_prep_mutex);
	int overlay_ready = frame_prep.overlay_ready;
	SDL_Surface* loaded_overlay = frame_prep.loaded_overlay;
	pthread_mutex_unlock(&video_prep_mutex);

	if (overlay_ready) {
		if(loaded_overlay) {
			if(!overlay_tex) glGenTextures(1, &overlay_tex);
			glBindTexture(GL_TEXTURE_2D, overlay_tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, loaded_overlay->w, loaded_overlay->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, loaded_overlay->pixels);
			overlay_w = loaded_overlay->w;
			overlay_h = loaded_overlay->h;

		} else {
			if (overlay_tex) {
				glDeleteTextures(1, &overlay_tex);
			}
			overlay_tex = 0;
		}
		pthread_mutex_lock(&video_prep_mutex);
        frame_prep.overlay_ready = 0;
		pthread_mutex_unlock(&video_prep_mutex);
    }

	if (!src_texture || reloadShaderTextures) {
        // if (src_texture) {
        //     glDeleteTextures(1, &src_texture);
        //     src_texture = 0;
        // }
		if (src_texture==0)
        	glGenTextures(1, &src_texture);
        glBindTexture(GL_TEXTURE_2D, src_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nrofshaders > 0 ? shaders[0]->filter : finalScaleFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nrofshaders > 0 ? shaders[0]->filter : finalScaleFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glBindTexture(GL_TEXTURE_2D, src_texture);
    if (vid.blit->src_w != src_w_last || vid.blit->src_h != src_h_last || reloadShaderTextures) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vid.blit->src_w, vid.blit->src_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, vid.blit->src);
        src_w_last = vid.blit->src_w;
        src_h_last = vid.blit->src_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vid.blit->src_w, vid.blit->src_h, GL_RGBA, GL_UNSIGNED_BYTE, vid.blit->src);
    }

    last_w = vid.blit->src_w;
    last_h = vid.blit->src_h;

    SDL_Rect effect_rect = (SDL_Rect){dst_rect.x, dst_rect.y, effect_w, effect_h};

    for (int i = 0; i < nrofshaders; i++) {
        int src_w = last_w;
        int src_h = last_h;
        int dst_w = src_w * shaders[i]->scale;
        int dst_h = src_h * shaders[i]->scale;

        if (shaders[i]->scale == 9) {
            dst_w = dst_rect.w;
            dst_h = dst_rect.h;
        }

        if (reloadShaderTextures) {
            for (int j = i; j < nrofshaders; j++) {
                int real_input_w = (i == 0) ? vid.blit->src_w : last_w;
                int real_input_h = (i == 0) ? vid.blit->src_h : last_h;

                shaders[i]->srcw = shaders[i]->srctype == 0 ? vid.blit->src_w : shaders[i]->srctype == 2 ? dst_rect.w : real_input_w;
                shaders[i]->srch = shaders[i]->srctype == 0 ? vid.blit->src_h : shaders[i]->srctype == 2 ? dst_rect.h : real_input_h;
                shaders[i]->texw = shaders[i]->scaletype == 0 ? vid.blit->src_w : shaders[i]->scaletype == 2 ? dst_rect.w : real_input_w;
                shaders[i]->texh = shaders[i]->scaletype == 0 ? vid.blit->src_h : shaders[i]->scaletype == 2 ? dst_rect.h : real_input_h;
            }
        }

        static int shaderinfocount = 0;
        static int shaderinfoscreen = 0;
        if (shaderinfocount > 600 && shaderinfoscreen == i) {
            currentshaderpass = i + 1;
            currentshadertexw = shaders[i]->texw;
            currentshadertexh = shaders[i]->texh;
            currentshadersrcw = shaders[i]->srcw;
            currentshadersrch = shaders[i]->srch;
            currentshaderdstw = dst_w;
            currentshaderdsth = dst_h;
            shaderinfocount = 0;
            shaderinfoscreen++;
            if (shaderinfoscreen >= nrofshaders)
                shaderinfoscreen = 0;
        }
        shaderinfocount++;

        if (shaders[i]->shader_p) {
			//LOG_info("Shader Pass: Pipeline step %d/%d\n", i + 1, nrofshaders);
            runShaderPass(
                (i == 0) ? src_texture : shaders[i - 1]->texture,
                shaders[i]->shader_p,
                &shaders[i]->texture,
                0, 0, dst_w, dst_h,
                shaders[i],
                0,
                (i == nrofshaders - 1) ? finalScaleFilter : shaders[i + 1]->filter
            );
        } else {
            runShaderPass(
                (i == 0) ? src_texture : shaders[i - 1]->texture,
                g_noshader,
                &shaders[i]->texture,
                0, 0, dst_w, dst_h,
                shaders[i],
                0,
                (i == nrofshaders - 1) ? finalScaleFilter : shaders[i + 1]->filter
            );
        }

        last_w = dst_w;
        last_h = dst_h;
    }

    if (nrofshaders > 0) {
		//LOG_info("Shader Pass: Scale to screen (pipeline size: %d)\n", nrofshaders);
        runShaderPass(
            shaders[nrofshaders - 1]->texture,
            g_shader_default,
            NULL,
            dst_rect.x, dst_rect.y, dst_rect.w, dst_rect.h,
            &(Shader){.srcw = last_w, .srch = last_h, .texw = last_w, .texh = last_h},
            0, GL_NONE
        );
    }
	else {
		//LOG_info("Shader Pass: Scale to screen (pipeline size: %d)\n", nrofshaders);
        runShaderPass(src_texture,
			g_shader_default,
			NULL,
			dst_rect.x, dst_rect.y, dst_rect.w, dst_rect.h,
            &(Shader){.srcw = vid.blit->src_w, .srch = vid.blit->src_h, .texw = vid.blit->src_w, .texh = vid.blit->src_h},
            0, GL_NONE);
    }

    if (effect_tex) {
		//LOG_info("Shader Pass: Screen Effect\n");
        runShaderPass(
            effect_tex,
            g_shader_overlay,
            NULL,
            effect_rect.x, effect_rect.y, effect_rect.w, effect_rect.h,
            &(Shader){.srcw = effect_w, .srch = effect_h, .texw = effect_w, .texh = effect_h},
            1, GL_NONE
        );
    }

    if (overlay_tex) {
		//LOG_info("Shader Pass: Overlay\n");
        runShaderPass(
            overlay_tex,
            g_shader_overlay,
            NULL,
            dst_rect.x, dst_rect.y, dst_rect.w, dst_rect.h,
            &(Shader){.srcw = vid.blit->src_w, .srch = vid.blit->src_h, .texw = overlay_w, .texh = overlay_h},
            1, GL_NONE
        );
    }

    // Render notification overlay if present (texture pre-allocated in PLAT_initShaders)
    if (notif.dirty && notif.surface) {
        glBindTexture(GL_TEXTURE_2D, notif.tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, notif.surface->w, notif.surface->h, GL_RGBA, GL_UNSIGNED_BYTE, notif.surface->pixels);
        notif.dirty = 0;
    }

    if (notif.tex && notif.surface) {
        runShaderPass(
            notif.tex,
            g_shader_overlay,
            NULL,
            notif.x, notif.y, notif.tex_w, notif.tex_h,
            &(Shader){.srcw = notif.tex_w, .srch = notif.tex_h, .texw = notif.tex_w, .texh = notif.tex_h},
            1, GL_NONE
        );
    }

	SDL_GL_SwapWindow(vid.window);

    frame_count++;
	reloadShaderTextures = 0;
	shaderResetRequested = 0;

	//{
	//	uint64_t op_ts = SDL_GetPerformanceCounter();
	//	uint64_t frame_duration = op_ts - frame_start;
	//	frame_start = op_ts;
	//	double elapsed_time_s = (double)frame_duration / performance_frequency;
	//	double frame_ms = elapsed_time_s * 1000.0;
	//	LOG_info("10: %.2f ms\n", frame_ms);
	//}
}

// flipping image upside down
void PLAT_pixelFlipper(uint8_t* pixels, int width, int height) {
    const int rowBytes = width * 4;
    uint8_t* rowTop;
    uint8_t* rowBottom;

    for (int y = 0; y < height / 2; ++y) {
        rowTop = pixels + y * rowBytes;
        rowBottom = pixels + (height - 1 - y) * rowBytes;

        int x = 0;
// NEON optimization for compatible ARM architectures
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        for (; x + 15 < rowBytes; x += 16) {
            uint8x16_t top = vld1q_u8(rowTop + x);
            uint8x16_t bottom = vld1q_u8(rowBottom + x);

            vst1q_u8(rowTop + x, bottom);
            vst1q_u8(rowBottom + x, top);
        }
#endif
        for (; x < rowBytes; ++x) {
            uint8_t temp = rowTop[x];
            rowTop[x] = rowBottom[x];
            rowBottom[x] = temp;
        }
    }
}

unsigned char* PLAT_GL_screenCapture(int* outWidth, int* outHeight) {
    glViewport(0, 0, device_width, device_height);

    if (outWidth)  *outWidth  = device_width;
    if (outHeight) *outHeight = device_height;

    unsigned char* pixels = malloc(device_width * device_height * 4); // RGBA
    if (!pixels) return NULL;

    glReadPixels(0, 0, device_width, device_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    PLAT_pixelFlipper(pixels, device_width, device_height);

    return pixels; // caller must free
}
