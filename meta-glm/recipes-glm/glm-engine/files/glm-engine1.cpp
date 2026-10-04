#define EGL_NO_X11
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <map>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <list>
#include <new>
#include <sys/stat.h> // 🚀 لإدارة المجلدات والملفات
#include <pthread.h> // 🚀 للتحكم المباشر في مسارات المعالج
#include <sched.h>   // 🚀 للتحكم في أولوية الجدولة بنظام لينكس
#include <dlfcn.h>   // 🚀 للتحميل الديناميكي للمكتبات المغلقة (Dynamic Loading)

#include <curl/curl.h>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// 🚀 إضافة مكتبة التصغير السريعة
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"

#define NUM_LOADER_THREADS 3 // عدد العمال المتزامنين (سرعة خيالية)

// 🚀 إضافة مكتبات المحرك الاحترافي
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <stdarg.h>
#include <stdio.h>

// ==================================================================================
// 📋 نظام السجل (Logging)
// ==================================================================================
// في الإصدار العادي GLM_LOG لا تُنتج أي كود إطلاقاً (صفر تكلفة).
// للتشخيص: صرّف بـ -DGLM_DEBUG. حتى في وضع الديبوج نفتح الملف مرة واحدة فقط
// ولا نستدعي fsync أبداً — كان fsync يحجب مسار الرسم عشرات الميلي ثانية على فلاش الجهاز.
// ==================================================================================
#ifdef GLM_DEBUG
static const char* GLM_LOG_PATH = "/home/root/glm_debug.log";
static std::mutex glm_log_mutex;

static void glm_debug_to_file(const char* format, ...) {
    static FILE* logFile = nullptr;                 // يُفتح مرة واحدة ويبقى مفتوحاً
    std::lock_guard<std::mutex> lk(glm_log_mutex);  // السجل يُستدعى من عدة مسارات
    if (!logFile) {
        logFile = fopen(GLM_LOG_PATH, "a");
        if (!logFile) return;
        setvbuf(logFile, nullptr, _IOLBF, 4096);    // تفريغ عند كل سطر، بلا fsync
    }
    va_list args;
    va_start(args, format);
    vfprintf(logFile, format, args);
    va_end(args);
}
#define GLM_LOG(...) glm_debug_to_file(__VA_ARGS__)
#else
#define GLM_LOG(...) ((void)0)
#endif

// ==================================================================================
// 🔒 ترتيب الأقفال (Lock Ordering) — قاعدة إلزامية
// ==================================================================================
// إذا احتجت قفلين معاً، اقفل دائماً بهذا الترتيب ولا تعكسه أبداً:
//
//        screenMutex  ←  queueMutex  ←  maskCacheMutex
//        queueMutex   ←  renderCvMutex        (عبر glm_request_render فقط)
//
// عكس الترتيب في أي موضع = جمود (deadlock) يصعب جداً تشخيصه على الجهاز.
// volumeStyleMutex و glTaskMutex مستقلان ولا يُؤخذان مع غيرهما إطلاقاً.
// شرط انتظار renderCv يقرأ متغيرات ذرّية فقط، فلا يحتاج أي قفل آخر.
// ==================================================================================

// 🚀 إعدادات الكاش الدائم (Disk Cache)
// المسار الافتراضي على الهاردسك، مع تراجع تلقائي إلى /tmp إذا لم يكن قابلاً للكتابة.
std::string CACHE_DIR = "/media/hdd/ev_cache/";

// // دالة لتحويل الرابط إلى اسم ملف آمن (Unique Filename)
std::string url_to_filename(std::string url) {
    std::string filename = url;
    for (auto &c : filename) {
        if (!isalnum(c)) c = '_';
    }
    if (filename.length() > 100) filename = filename.substr(filename.length() - 100);
    return CACHE_DIR + filename + ".img";
}

// إنشاء شجرة مجلدات كاملة (ما يعادل mkdir -p)
static bool make_dir_tree(const std::string& path) {
    if (path.empty()) return false;
    std::string acc;
    for (size_t i = 0; i < path.size(); ++i) {
        acc += path[i];
        if (path[i] == '/' && acc.size() > 1) {
            if (mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    if (path.back() != '/') {
        if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

// اختبار حقيقي لقابلية الكتابة (وجود المجلد لا يعني أنه قابل للكتابة)
static bool dir_is_writable(const std::string& dir) {
    std::string probe = dir + ".glm_write_test";
    FILE* f = fopen(probe.c_str(), "wb");
    if (!f) return false;
    fclose(f);
    unlink(probe.c_str());
    return true;
}

// دالة للتأكد من وجود مجلد الكاش، مع تراجع آمن إلى /tmp
void ensure_cache_dir() {
    if (!CACHE_DIR.empty() && CACHE_DIR.back() != '/') CACHE_DIR += '/';

    struct stat st;
    if (stat(CACHE_DIR.c_str(), &st) == -1) make_dir_tree(CACHE_DIR);

    if (!dir_is_writable(CACHE_DIR)) {
        GLM_LOG("[GLM] cache dir '%s' not writable, falling back to /tmp\n", CACHE_DIR.c_str());
        CACHE_DIR = "/tmp/ev_cache/";
        make_dir_tree(CACHE_DIR);
    }
}

// 🚀 هيكل بيانات جديد لربط الصورة المحملة بحالة الاستخدام
struct DownloadData {
    std::string* buffer;
    std::atomic<bool>* inUse; // 🚀 إصلاح: مؤشر إلى متغير ذري (atomic)
};

// ✅ متغيرات التحميل السريع للخلفية الديناميكية (لإزالة الثقل تماماً)
std::atomic<bool> bgTaskPending{false};
std::string bgTaskPath = "";
std::atomic<unsigned char*> bgTaskData{nullptr};
std::string bgTaskReadyPath = ""; // 🚀 مسار الصورة التي أصبحت بياناتها جاهزة
// 🎭 القناع المرتبط بطلب الباكدروب نفسه (يُلتقط لحظة الطلب لا لحظة الخَبز)
std::string bgTaskMaskPath  = "";
std::string bgTaskReadyMask = "";
std::atomic<int> bgTaskW{0};
std::atomic<int> bgTaskH{0};
// 🚀 مسار mask_normal لدمجه مع الباكدروب مرة واحدة عند تحميل الخلفية بدلاً من رسم PNG fullscreen كل فريم.
std::string backdropMaskPath = "";

// 👇 استبدل الدالتين بهذين التعديلين الآمنين 👇

size_t BackdropWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    // ⚠️ حذفنا الـ Kill Switch العنيف لمنع الكراش
    std::string* buffer = (std::string*)userp;
    if (!buffer) return 0;
    buffer->append((char*)contents, size * nmemb);
    return size * nmemb;
}

size_t ImageWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    DownloadData* data = (DownloadData*)userp;
    if (!data || !data->buffer) return 0;
    
    // 🚀 قراءة آمنة: إذا لم يعد مستخدماً لا نقطع بعنف بل نخرج بأمان
    if (data->inUse && !data->inUse->load()) {
        return size * nmemb; // نرجعه كأنه قُرئ لكن لا نحفظه بالـ buffer لتوفير الرام
    }
    
    data->buffer->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// 🚀 ذاكرة الكاش السريعة للخطوط (RAM Font Cache) لتجنب القراءة من الديسك تماماً
std::map<std::string, std::pair<unsigned char*, long>> fontBufferCache;

// 🔒 يُستدعى من مسار الرسم فقط (كل إنشاء نصوص يمر عبر طابور glTaskQueue).
unsigned char* get_cached_font(const std::string& path, long& sizeOut) {
    auto it = fontBufferCache.find(path);
    if (it != fontBufferCache.end()) {
        sizeOut = it->second.second;
        return it->second.first;
    }

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { GLM_LOG("[GLM] font not found: %s\n", path.c_str()); return nullptr; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return nullptr; }
    long fSize = ftell(f);
    // حماية: ftell قد يعيد -1، وملف فارغ أو ضخم بشكل غير منطقي يجب رفضه
    if (fSize <= 0 || fSize > (64L * 1024L * 1024L)) {
        GLM_LOG("[GLM] bad font size %ld for %s\n", fSize, path.c_str());
        fclose(f);
        return nullptr;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return nullptr; }

    unsigned char* buf = new (std::nothrow) unsigned char[fSize];
    if (!buf) { fclose(f); return nullptr; }

    size_t got = fread(buf, 1, (size_t)fSize, f);
    fclose(f);
    if (got != (size_t)fSize) {                 // قراءة ناقصة = خط تالف
        GLM_LOG("[GLM] short read on font %s (%zu/%ld)\n", path.c_str(), got, fSize);
        delete[] buf;
        return nullptr;
    }

    fontBufferCache[path] = std::make_pair(buf, fSize);
    sizeOut = fSize;
    return buf;
}

// 🚀 ذاكرة الكاش الخاصة بمحرك FreeType و HarfBuzz
FT_Library ft_lib;
std::map<std::string, FT_Face> ft_face_cache;
std::map<std::string, hb_font_t*> hb_font_cache;

FT_Face get_ft_face(const std::string& path) {
    if (ft_face_cache.find(path) != ft_face_cache.end()) return ft_face_cache[path];
    long size; unsigned char* buf = get_cached_font(path, size);
    if (!buf) return nullptr;
    FT_Face face;
    if (FT_New_Memory_Face(ft_lib, buf, size, 0, &face)) return nullptr;
    ft_face_cache[path] = face;
    hb_font_cache[path] = hb_ft_font_create(face, NULL);
    return face;
}

// 🚀 إنشاء هيكل الكاش العالمي لنصوص كرت الشاشة (Text Texture Cache)
struct CachedText {
    GLuint texId;
    int w;
    int h;
    int firstLineW;
    int firstLineH;

    // ملاحظة C++11: المُهيّئات داخل الصنف تلغي كون النوع "تجميعياً" (aggregate)،
    // فلا يعمل CachedText{a,b,c,d,e}. لذلك نعرّف بانيَين صريحين.
    CachedText() : texId(0), w(0), h(0), firstLineW(0), firstLineH(0) {}
    CachedText(GLuint id, int ww, int hh, int flw, int flh)
        : texId(id), w(ww), h(hh), firstLineW(flw), firstLineH(flh) {}
};

// 🔗 يُعرَّف بعد currentScreen: يمسح أي عنصر ما زال يشير إلى نسيج تمت إزاحته من الكاش،
//    حتى لا يبقى مؤشر معلّق (dangling texture id) في الواجهة بعد الإخلاء.
//    ⚠️ الكلفة O(عدد العناصر)، لذلك نمررها **دفعة واحدة** لكل عملية إخلاء
//    بدل استدعائها لكل نسيج على حدة (كانت تمسح آلاف العناصر لكل نص جديد).
static void glm_forget_text_textures(const std::vector<GLuint>& ids);

// ==================================================================================
// كاش نصوص الـ GPU بحد أقصى (LRU)
// ==================================================================================
// الكاش هو **المالك الوحيد** لهذه النسج: العناصر تستعير الـ ID ولا تحذفه أبداً.
// عند تجاوز الحد نزيح الأقدم استخداماً، وقبل حذف النسيج نصفّر أي عنصر يشير إليه.
// 🔒 يُستخدم من مسار الرسم فقط.
// ==================================================================================
class TextTextureCache {
public:
    // حد أعلى مرتفع: نسج النصوص صغيرة جداً، والإخلاء المتكرر أغلى من الاحتفاظ بها.
    static const size_t MAX_ENTRIES = 3072;

    // يعيد nullptr إذا لم يوجد. الوصول الناجح يرفع المدخل إلى قمة الاستخدام.
    const CachedText* get(const std::string& key) {
        std::unordered_map<std::string, Entry>::iterator it = map_.find(key);
        if (it == map_.end()) return NULL;
        order_.splice(order_.end(), order_, it->second.pos);
        return &it->second.data;
    }

    void put(const std::string& key, const CachedText& value) {
        std::unordered_map<std::string, Entry>::iterator it = map_.find(key);
        if (it != map_.end()) {
            if (it->second.data.texId != 0 && it->second.data.texId != value.texId) {
                std::vector<GLuint> one(1, it->second.data.texId);
                glm_forget_text_textures(one);
                glDeleteTextures(1, &it->second.data.texId);
            }
            it->second.data = value;
            order_.splice(order_.end(), order_, it->second.pos);
            return;
        }
        order_.push_back(key);
        Entry e;
        e.data = value;
        e.pos  = order_.end();
        --e.pos;
        map_.insert(std::make_pair(key, e));
        evict_if_needed();
    }

    // إخلاء كامل — يُستدعى عند تحميل شاشة جديدة أو عند إغلاق المحرك
    void clear() {
        if (map_.empty()) return;
        std::vector<GLuint> ids;
        ids.reserve(map_.size());
        for (std::unordered_map<std::string, Entry>::iterator it = map_.begin(); it != map_.end(); ++it) {
            if (it->second.data.texId != 0) ids.push_back(it->second.data.texId);
        }
        map_.clear();
        order_.clear();
        if (!ids.empty()) {
            glm_forget_text_textures(ids);                 // مسحة واحدة للمشهد كله
            glDeleteTextures((GLsizei)ids.size(), &ids[0]); // حذف دفعة واحدة
        }
    }

    // تصفير المراجع فقط بلا استدعاءات OpenGL (بعد تدمير السياق تصبح النسج معدومة أصلاً)
    void drop_without_gl() {
        map_.clear();
        order_.clear();
    }

    size_t size() const { return map_.size(); }

private:
    struct Entry {
        CachedText data;
        std::list<std::string>::iterator pos;
    };

    // نزيح دفعة كاملة مرة واحدة بدل مدخل واحد في كل إدراج،
    // فتصبح المسحة المكلفة نادرة جداً بدل أن تحدث مع كل نص جديد.
    void evict_if_needed() {
        if (map_.size() <= MAX_ENTRIES) return;

        const size_t target = (MAX_ENTRIES * 3) / 4;   // ننزل إلى 75% من الحد
        std::vector<GLuint> ids;
        while (map_.size() > target && !order_.empty()) {
            const std::string oldest = order_.front();
            std::unordered_map<std::string, Entry>::iterator it = map_.find(oldest);
            if (it != map_.end()) {
                if (it->second.data.texId != 0) ids.push_back(it->second.data.texId);
                map_.erase(it);
            }
            order_.pop_front();
        }
        if (!ids.empty()) {
            glm_forget_text_textures(ids);                 // مسحة واحدة لكل الدفعة
            glDeleteTextures((GLsizei)ids.size(), &ids[0]);
        }
    }

    std::unordered_map<std::string, Entry> map_;
    std::list<std::string> order_;   // الأقدم في الأمام، الأحدث في النهاية
};

// تعريف خارج الصنف: مطلوب في C++11 إذا استُخدم الثابت بطريقة تعتبره odr-use
const size_t TextTextureCache::MAX_ENTRIES;

TextTextureCache globalTextCache;

// ==================================================================================
// 🏷️ تعريفات الأنيميشن والاتجاه كأرقام بدل سلاسل نصية
// ==================================================================================
// كانت حلقة الرسم تقارن std::string لكل عنصر في كل إطار
// (‏orientation == "grid" وحدها 14 مرة). الآن مقارنة بايت واحد.
// السلاسل تبقى كما هي في XML و في واجهة بايثون؛ التحويل يحدث مرة واحدة فقط.
// ==================================================================================
enum class AnimId : uint8_t {
    None = 0,
    SlideUp, SlideUpFade, SlideDown, SlideDownFade,
    SlideLeft, SlideLeftFade, SlideRight, SlideRightFade,
    SlideOutUp, SlideOutDown, SlideOutLeft, SlideOutRight,
    ShiftRight, ShiftDown, ShiftBackLeft, ShiftBackUp,
    // 🌫️ نفس حركات shift لكن مع تلاشٍ محكوم بنفس منحنى الحركة بالضبط
    ShiftRightFade, ShiftDownFade, ShiftBackLeftFade, ShiftBackUpFade,
    // 🌫️ تلاشٍ خالص بلا أي إزاحة (للأغطية والتعتيم الملء-شاشة)
    FadeIn, FadeOut,
    MicroSlideLeft, ZoomFade, ZoomInFadeOut,
    Unknown
};

enum class OrientId : uint8_t { Vertical = 0, Horizontal, Grid };
enum class ItemShapeId : uint8_t { Rect = 0, Circle };
enum class AlignId : uint8_t { Left = 0, Center, Right, Block };

// ==================================================================================
// 🎚️ منحنيات التنعيم (Easing) لحركات الشاشة/الطبقة
// كلها من نوع "Out": تبدأ بأقصى سرعة ثم تتباطأ حتى تقف بسلاسة (السرعة = 0 عند النهاية).
// تُختار من الـ XML: <Layout ... Easing="expo">
// ==================================================================================
enum class EaseId : uint8_t { Quint = 0, Quart, Cubic, Quad, Sine, Expo, Circ, Back, Linear };

static EaseId ease_id_of(const std::string& s) {
    static const std::unordered_map<std::string, EaseId> table = {
        {"quint",  EaseId::Quint},  {"out_quint",  EaseId::Quint},
        {"quart",  EaseId::Quart},  {"out_quart",  EaseId::Quart},
        {"cubic",  EaseId::Cubic},  {"out_cubic",  EaseId::Cubic},
        {"quad",   EaseId::Quad},   {"out_quad",   EaseId::Quad},
        {"sine",   EaseId::Sine},   {"out_sine",   EaseId::Sine},
        {"expo",   EaseId::Expo},   {"out_expo",   EaseId::Expo},
        {"circ",   EaseId::Circ},   {"out_circ",   EaseId::Circ},
        {"back",   EaseId::Back},   {"out_back",   EaseId::Back},
        {"linear", EaseId::Linear},
    };
    auto it = table.find(s);
    return (it == table.end()) ? EaseId::Quint : it->second;   // الافتراضي = السلوك القديم
}

// t في [0,1] ⇒ التقدّم المنعَّم في [0,1]
static inline float glm_ease_out(EaseId id, float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float u = 1.0f - t;
    switch (id) {
        case EaseId::Linear: return t;
        case EaseId::Quad:   return 1.0f - u * u;
        case EaseId::Cubic:  return 1.0f - u * u * u;
        case EaseId::Quart:  return 1.0f - u * u * u * u;
        case EaseId::Sine:   return (float)sin((double)t * 1.5707963268);
        // مُعايَر ليصل 1.0 بالضبط عند t=1 فلا تحدث قفزة في آخر إطار
        case EaseId::Expo:   return (float)((1.0 - pow(2.0, -10.0 * (double)t)) / 0.9990234375);
        case EaseId::Circ:   return (float)sqrt(1.0 - (double)u * (double)u);
        case EaseId::Back: { const float c1 = 1.70158f, c3 = c1 + 1.0f;
                             return 1.0f + c3 * u * u * u - c1 * u * u; }
        case EaseId::Quint:
        default:             return 1.0f - u * u * u * u * u;
    }
}

static AnimId anim_id_of(const std::string& s) {
    if (s.empty()) return AnimId::None;
    static const std::unordered_map<std::string, AnimId> table = {
        {"none",              AnimId::None},
        {"slide_up",          AnimId::SlideUp},
        {"slide_up_fade",     AnimId::SlideUpFade},
        {"slide_down",        AnimId::SlideDown},
        {"slide_down_fade",   AnimId::SlideDownFade},
        {"slide_left",        AnimId::SlideLeft},
        {"slide_left_fade",   AnimId::SlideLeftFade},
        {"slide_right",       AnimId::SlideRight},
        {"slide_right_fade",  AnimId::SlideRightFade},
        {"slide_out_up",      AnimId::SlideOutUp},
        {"slide_out_down",    AnimId::SlideOutDown},
        {"slide_out_left",    AnimId::SlideOutLeft},
        {"slide_out_right",   AnimId::SlideOutRight},
        {"shift_right",       AnimId::ShiftRight},
        {"shift_down",        AnimId::ShiftDown},
        {"shift_back_left",   AnimId::ShiftBackLeft},
        {"shift_back_up",     AnimId::ShiftBackUp},
        // 🌫️ الإصدارات المتلاشية: الإزاحة والشفافية يقودهما نفس animT
        {"shift_right_fade",     AnimId::ShiftRightFade},
        {"shift_down_fade",      AnimId::ShiftDownFade},
        {"shift_back_left_fade", AnimId::ShiftBackLeftFade},
        {"shift_back_up_fade",   AnimId::ShiftBackUpFade},
        {"fade_in",              AnimId::FadeIn},
        {"fade_out",             AnimId::FadeOut},
        {"micro_slide_left",  AnimId::MicroSlideLeft},
        {"zoom_fade",         AnimId::ZoomFade},
        {"zoom_in_fade_out",  AnimId::ZoomInFadeOut},
    };
    auto it = table.find(s);
    return (it == table.end()) ? AnimId::Unknown : it->second;
}

static OrientId orient_id_of(const std::string& s) {
    if (s == "grid")       return OrientId::Grid;
    if (s == "horizontal") return OrientId::Horizontal;
    return OrientId::Vertical;
}

static ItemShapeId shape_id_of(const std::string& s) {
    return (s == "circle") ? ItemShapeId::Circle : ItemShapeId::Rect;
}

static AlignId align_id_of(const std::string& s) {
    if (s == "center") return AlignId::Center;
    if (s == "right")  return AlignId::Right;
    if (s == "block")  return AlignId::Block;
    return AlignId::Left;
}

// الأنيميشنات التي تستخدم كاميرا الزووم بدل الكاميرا الثابتة
static inline bool anim_uses_zoom_camera(AnimId a) {
    return a == AnimId::ZoomFade || a == AnimId::ZoomInFadeOut;
}

// ==================================================================================
// 🎛️ كاش حالة OpenGL
// ==================================================================================
// كان مسار الرسم يستدعي glUseProgram و glEnable(GL_BLEND) عشرات المرات بقيم مكررة.
// كل استدعاء متكرر = رحلة إلى السائق بلا فائدة.
// ==================================================================================
static GLuint  gl_cur_program = 0;
static int     gl_cur_blend   = -1;   // -1 غير معروف، 0 مطفأ، 1 مفعّل

static inline void gl_use_program(GLuint p) {
    if (p != gl_cur_program) { glUseProgram(p); gl_cur_program = p; }
}
static inline void gl_set_blend(bool on) {
    int want = on ? 1 : 0;
    if (want != gl_cur_blend) {
        if (on) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        gl_cur_blend = want;
    }
}
// تُستدعى عند كل تبديل framebuffer أو إعادة بناء السياق
static inline void gl_state_cache_reset() { gl_cur_program = 0; gl_cur_blend = -1; }

// ==================================================================================
// 🧪 تصريف الشادرات مع فحص الأخطاء
// ==================================================================================
// لم يكن في المحرك أي glGetShaderiv / glGetProgramiv: فشل شادر على جهاز معيّن
// كان ينتج شاشة سوداء صامتة بلا أي أثر في السجل.
// ==================================================================================
static GLuint glm_compile_shader(GLenum type, const char* src, const char* name) {
    GLuint s = glCreateShader(type);
    if (s == 0) { GLM_LOG("[GLM-SHADER] glCreateShader failed for %s\n", name); return 0; }
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);

    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint logLen = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log((logLen > 1) ? logLen : 1, '\0');
        glGetShaderInfoLog(s, (GLsizei)log.size(), nullptr, log.data());
        GLM_LOG("[GLM-SHADER] COMPILE FAILED (%s):\n%s\n", name, log.data());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// يبني برنامجاً كاملاً ويحرر الشادرات بعده (كانت تتسرب في كل دورة فتح/إغلاق)
static GLuint glm_build_program(const char* vsSrc, const char* fsSrc, const char* name) {
    GLuint vs = glm_compile_shader(GL_VERTEX_SHADER, vsSrc, name);
    if (!vs) return 0;
    GLuint fs = glm_compile_shader(GL_FRAGMENT_SHADER, fsSrc, name);
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint prog = glCreateProgram();
    if (prog == 0) { glDeleteShader(vs); glDeleteShader(fs); return 0; }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint logLen = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log((logLen > 1) ? logLen : 1, '\0');
        glGetProgramInfoLog(prog, (GLsizei)log.size(), nullptr, log.data());
        GLM_LOG("[GLM-SHADER] LINK FAILED (%s):\n%s\n", name, log.data());
        glDetachShader(prog, vs); glDetachShader(prog, fs);
        glDeleteShader(vs); glDeleteShader(fs);
        glDeleteProgram(prog);
        return 0;
    }

    // بعد الربط لم تعد الشادرات مطلوبة — فصلها وحذفها يمنع التسريب
    glDetachShader(prog, vs);
    glDetachShader(prog, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLM_LOG("[GLM-SHADER] program '%s' built OK (id=%u)\n", name, prog);
    return prog;
}

// ماكرو محمول بديل عن [[maybe_unused]] (من C++17) — الوصفة تصرّف بـ -std=c++11
#if defined(__GNUC__) || defined(__clang__)
#  define GLM_MAYBE_UNUSED __attribute__((unused))
#else
#  define GLM_MAYBE_UNUSED
#endif

// ==================================================================================
// 📄 قراءة سمات XML بلا إزاحات سحرية
// ==================================================================================
// كان المحلل يستخدم إزاحات ثابتة مثل  size_t s = p + 11;  حيث 11 يجب أن يطابق
// طول "cornerDia=\"" بالضبط. أي تعديل في اسم السمة كان ينتج خطأ تحليل صامتاً.
// GLM_ATTR_LEN يحسب الطول من نفس النص الحرفي وقت التصريف، فلا مجال للانحراف.
// ==================================================================================
#define GLM_ATTR_LEN(key) (sizeof(key "=\"") - 1)

// الشكل المفضّل للكود الجديد: استخراج آمن لقيمة سمة من وسم
GLM_MAYBE_UNUSED static bool xml_attr(const std::string& tag, const char* key, std::string& out) {
    const std::string needle = std::string(key) + "=\"";
    size_t kp = tag.find(needle);
    if (kp == std::string::npos) return false;
    size_t st = kp + needle.size();
    size_t en = tag.find('"', st);
    if (en == std::string::npos) return false;
    out = tag.substr(st, en - st);
    return true;
}

GLM_MAYBE_UNUSED static bool xml_attr_float(const std::string& tag, const char* key, float& out) {
    std::string v;
    if (!xml_attr(tag, key, v)) return false;
    try { out = std::stof(v); return true; } catch (...) { return false; }
}

GLM_MAYBE_UNUSED static bool xml_attr_int(const std::string& tag, const char* key, int& out) {
    std::string v;
    if (!xml_attr(tag, key, v)) return false;
    try { out = std::stoi(v); return true; } catch (...) { return false; }
}

// أقصى مقاس نسيج يدعمه الجهاز — يُقرأ مرة واحدة بعد إنشاء السياق
static GLint glm_max_texture_size = 2048;
static void glm_query_limits() {
    GLint v = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
    if (v >= 64) glm_max_texture_size = v;
    GLM_LOG("[GLM] GL_MAX_TEXTURE_SIZE = %d\n", glm_max_texture_size);
}

// 🚀 متغيرات السپينر العالمي
GLuint texLoadingBack = 0;
GLuint texLoadingSpinner = 0;

// ==================================================================================
// 🚀 FBO Static Snapshot Cache
// يلتقط آخر فريم مستقر كصورة واحدة ثم يعيد رسمها بدل إعادة رسم الخلفية/البوسترات
// هذا يقلل استهلاك الـ GPU عندما تكون الشاشة ثابتة ولا توجد أنيميشن/سكرول/تحميل.
// ==================================================================================
GLuint staticSceneFbo = 0;
GLuint staticSceneTex = 0;
int staticSceneW = 0;
int staticSceneH = 0;
std::atomic<bool> staticSceneDirty{true};
bool staticSceneValid = false;

// 🚀 Layered cache: يجمّد كل الطبقات التي تحت القائمة المتحركة فقط.
// أثناء حركة القائمة/الكاروسال: الخلفية والبوسترات ترسم كصورة واحدة، والقائمة فقط تتحرك فوقها.
GLuint layerSceneFbo = 0;
GLuint layerSceneTex = 0;
int layerSceneW = 0;
int layerSceneH = 0;
int layerSceneSplitZ = 999999;
std::atomic<bool> layerSceneDirty{true};
bool layerSceneValid = false;

void invalidate_static_scene_cache() {
    staticSceneDirty.store(true);
    staticSceneValid = false;
}

void invalidate_layer_scene_cache() {
    layerSceneDirty.store(true);
    layerSceneValid = false;
}

// ==================================================================================
// 🚀 Threading System for Render Loop (Background Engine)
// ==================================================================================
std::atomic<bool> engine_running{false};
// وضع اختياري لبدء الواجهة بلا reset_fb وبلا انتظار بعد إنشاء EGL.
// يتم استهلاك القيمة مرة واحدة عند init حتى لا تنتقل إلى البلوجين التالي.
std::atomic<bool> seamless_startup_requested{false};
// 🚀 علم الإغلاق: بمجرد رفعه يتوقف المحرك عن رسم أي محتوى جديد.
// هذا يمنع عرض إطار نصف مبني أو إعادة بناء المشهد لحظة الخروج (مصدر الرمشة).
std::atomic<bool> engine_exiting{false};
// 🚀 زمن آخر إطار مرسوم؋ نعيد استعماله عند الإغلاق
// لنرسم إطاراً مطابقاً تماماً للأخير (بلا أي تقدم في الأنيميشن).
std::atomic<float> engine_last_frame_time{0.0f};
std::thread render_thread;
std::atomic<bool> force_render{false};
// ⏱️ يُرفع كلما نامت حلقة الرسم (لم يُرسم إطار). أول إطار بعد السكون يستعمل
//    زمناً اسمياً بدل الفجوة المتراكمة، وإلا قفز أول إطار حركة ضعف مسافته.
std::atomic<bool> physics_resync{true};

// ==================================================================================
// 🎬 فيد مؤجَّل يُطبَّق لحظة اكتمال تحميل الشاشة
// ==================================================================================
// المشكلة التي يحلّها: set_screen_fade أمر يدخل طابور المحرك، ومسار الرسم يعمل
// بأولوية SCHED_FIFO قصوى فيستبق مسار بايثون فور وصول أول أمر. النتيجة أن المحرك
// كان ينفّذ load_interface_xml ويرسم إطاراً كاملاً (فتظهر الشاشة) قبل أن يلحق
// بايثون بإرسال أمر الفيد، فيظهر السواد **بعد** الشاشة بدل أن يسبقها.
// لا يمكن لبايثون أن تكسب سباقاً ضد مسار FIFO 99، لذلك ننقل الضمان إلى المحرك:
// القيمة تُخزَّن هنا مسبقاً، و internal_load_interface_xml تستهلكها في **نفس المهمة**
// بعد بناء الشاشة مباشرة — فلا يمكن لأي إطار أن يُرسم بينهما مهما كان توقيت الجدولة.
std::atomic<int>   pending_screen_fade_type{0};
std::atomic<float> pending_screen_fade_duration{0.4f};
// 🔔 إشعار مسار الرسم بدل استطلاعه كل 16ms
std::condition_variable renderCv;
std::mutex             renderCvMutex;

// يُستدعى من أي مسار آخر لإيقاظ محرك الرسم فوراً.
// أخذ القفل وتحريره فوراً يمنع "الإشعار الضائع" لو كان المسار بين فحص الشرط والنوم.
inline void glm_request_render() {
    force_render.store(true);
    { std::lock_guard<std::mutex> lk(renderCvMutex); }
    renderCv.notify_one();
}
// ==================================================================================
// 📊 عدّادات تشخيص الأداء — تُطبع فقط عند وجود إطار بطيء (نادراً)
// ==================================================================================
std::atomic<int> g_perf_texts{0};    // كم نسيج نص وُلّد في هذا الإطار
std::atomic<int> g_perf_items{0};    // كم عنصر دخل حلقة الرسم
std::atomic<int> g_perf_drawn{0};    // كم عنصر رُسم فعلاً بعد القص
std::atomic<int> g_perf_present_us{0}; // زمن eglSwapBuffers/flush بالميكروثانية
std::atomic<int> g_perf_bg_us{0};      // زمن رسم خلفيات العناصر
std::atomic<int> g_perf_text_us{0};    // زمن رسم النصوص
std::atomic<long long> g_perf_px{0};   // مجموع البكسلات المرسومة (خلفيات العناصر)

std::atomic<bool> ui_hidden{false}; // 🚀 مفتاح التحكم في إخفاء الواجهة كلياً
std::atomic<bool> backdrop_hidden{false}; // 🚀 مفتاح التحكم في إخفاء الباكدروب فقط (للتريلر)
std::atomic<bool> last_backdrop_hidden{false}; // 🚀 لتتبع التغيرات وعمل الفيد
std::atomic<float> trailer_fade_start{-1.0f}; // 🚀 توقيت بداية الفيد
// 🚀 بعد رجوع الباكدروب من التريلر نستمر بالرسم لحظة قصيرة، لأن stopService في بايثون
// قد يحدث بعد fade بـ 550ms ويمسح طبقة الفيديو/الـ framebuffer فتظهر خلفية سوداء إن لم نرسم بعدها.
std::atomic<float> trailer_post_render_until{-1.0f};
// 🚀 Trailer Exit Cover: غطاء خلفي ثابت مثل Prime Video يغطي لحظة إيقاف طبقة الفيديو.
std::atomic<bool> trailer_cover_visible{false};
std::atomic<float> trailer_cover_alpha{0.0f};
std::atomic<float> trailer_cover_fade_start{-1.0f};

// ==================================================================================
// 🔊 GLM VOLUME OSD  -  شريط الصوت الخاص بمحركنا (بديل واجهة الإنيجما)
// ==================================================================================
struct VolumeBarStyle {
    bool  centerX = true;          // إذا كان true يتم توسيطه أفقياً
    float x = 0.0f;
    float y = 860.0f;
    float w = 620.0f;
    float h = 96.0f;
    float radius = 24.0f;          // نصف قطر زوايا اللوحة
    float barRadius = 6.0f;        // نصف قطر زوايا الشريط
    float barHeight = 0.0f;        // 0 = تلقائي
    float panelR = 0.078f, panelG = 0.094f, panelB = 0.121f, panelA = 0.94f;
    float trackR = 0.227f, trackG = 0.250f, trackB = 0.282f, trackA = 1.0f;
    float fillR  = 0.000f, fillG  = 0.749f, fillB  = 1.000f, fillA  = 1.0f;
    float textR  = 1.0f,   textG  = 1.0f,   textB  = 1.0f,   textA  = 1.0f;
    float muteR  = 0.541f, muteG  = 0.561f, muteB  = 0.596f, muteA  = 1.0f;
    float iconR  = 1.0f,   iconG  = 1.0f,   iconB  = 1.0f;
    std::string fontPath = "fonts/M-Bold.ttf";
    int fontSize = 30;
    std::string iconFontPath = "fonts/MaterialIcons-Regular.ttf";
    int iconFontSize = 44;
    uint32_t iconCp      = 0xE050; // volume_up
    uint32_t iconMutedCp = 0xE04F; // volume_off
    int showPercent = 1;
};

VolumeBarStyle volumeStyle;                 // الإعدادات الفعّالة
std::mutex volumeStyleMutex;                // حماية الإعدادات من تعدد المسارات
std::atomic<bool>  volume_style_from_python{false};

std::atomic<bool>  volume_active{false};    // هل الشريط معروض الآن؟
std::atomic<int>   volume_value{0};         // 0..100
std::atomic<bool>  volume_muted{false};
std::atomic<float> volume_show_start{-1.0f};   // لحظة الظهور (للفيد-إن)
std::atomic<float> volume_touch_time{-1.0f};   // آخر ضغطة (للإخفاء التلقائي)
std::atomic<float> volume_timeout{3.0f};       // مدة البقاء بالثواني
std::atomic<bool>  volume_fill_snap{true};     // أول ظهور: بدون تحريك التعبئة

// مسار الرسم فقط يلمس هذين المتغيرين
float volume_display_fill = 0.0f;
float volume_last_draw_time = -1.0f;

inline bool volume_overlay_active() { return volume_active.load(); }

// محلل ألوان #RRGGBB أو #RRGGBBAA (نفس صيغة باقي المحرك)
inline bool glm_parse_hex_color(const std::string& hex, float& r, float& g, float& b, float& a) {
    if (hex.size() < 7 || hex[0] != '#') return false;
    try {
        r = std::stoi(hex.substr(1, 2), nullptr, 16) / 255.0f;
        g = std::stoi(hex.substr(3, 2), nullptr, 16) / 255.0f;
        b = std::stoi(hex.substr(5, 2), nullptr, 16) / 255.0f;
        a = (hex.size() >= 9) ? (std::stoi(hex.substr(7, 2), nullptr, 16) / 255.0f) : 1.0f;
    } catch (...) { return false; }
    return true;
}

// 🎨 قراءة لون سمة كاملة من وسم XML مباشرة (يعتمد على المحلل أعلاه)
GLM_MAYBE_UNUSED static bool xml_attr_color(const std::string& tag, const char* key,
                                            float& r, float& g, float& b, float& a) {
    std::string v;
    if (!xml_attr(tag, key, v)) return false;
    std::string clean;
    for (char c : v) if (c != ' ' && c != '\t' && c != '\n' && c != '\r') clean += c;
    return glm_parse_hex_color(clean, r, g, b, a);
}

int engine_screen_w = 1920;
int engine_screen_h = 1080;
long long engine_start_time = 0;

std::queue<std::function<void()>> glTaskQueue;
std::mutex glTaskMutex;

// 🚀 Coalescing للضغطات المتتالية: نحتفظ بآخر selection فقط لكل Widget
// بدل تنفيذ 10 أوامر selection في نفس الفريم عند الضغط السريع.
std::mutex pendingSelectionMutex;
std::map<std::string, int> pendingWidgetSelections;
bool selectionTaskQueued = false;

void execute_gl_tasks() {
    // 🚀 تفريغ الطابور بسرعة البرق وتحرير القفل (Mutex) فوراً لمنع تجميد بايثون
    std::vector<std::function<void()>> tasks_to_run;
    {
        std::lock_guard<std::mutex> lock(glTaskMutex);
        while (!glTaskQueue.empty()) {
            tasks_to_run.push_back(glTaskQueue.front());
            glTaskQueue.pop();
        }
    }
    
    // تنفيذ المهام براحة تامة بدون إيقاف أي مسارات أخرى
    for (auto& task : tasks_to_run) {
        task();
    }
}

// 🚀 تحويل النظام إلى Fire-and-Forget (لاتزامني تماماً)
void run_on_gl_thread_async(std::function<void()> task) {
    // أي أمر قادم من بايثون قد يغيّر الشاشة الثابتة الكاملة.
    // لكن لا نلغي Layered FBO هنا، لأن set_widget_selection يتكرر مع كل ضغطة
    // وكان يعيد بناء طبقة الخلفية أثناء حركة الكاروسال ويسبب تقطيعاً متقطعاً.
    staticSceneDirty.store(true);
    {
        std::lock_guard<std::mutex> lock(glTaskMutex);
        glTaskQueue.push(task);
    }
    glm_request_render();  // تنبيه المحرك برفق
}

extern "C" {

    

    // ==================================================================================
    // 2. هياكل البيانات (Data Models)
    // ==================================================================================
    struct BaseElement {
        float x = 0, y = 0, w = 0, h = 0; int z = 0; float offsetX = 0.0f;
        std::string anim = ""; // ✅ أنيميشن خاص بهذا العنصر
        // 🏷️ نسخة رقمية من anim تُحدَّث مرة واحدة في بداية كل إطار
        //    (refresh_element_ids)، فلا نقارن سلاسل نصية داخل حلقة الرسم.
        AnimId animId = AnimId::None;
        float animDistance = 80.0f; // ✅ المسافة الافتراضية للانزلاق
        float animDuration = 0.8f; // 🚀 السرعة المستقلة لكل عنصر
        // 🌫️ تلاشٍ مستقل يُضاف فوق أي أنيميشن: 0=بلا، 1=دخول (0→1)، 2=خروج (1→0).
        //    يتقاسم نفس منحنى وتوقيت الحركة، ولا يمسّ منطق الإزاحة إطلاقاً.
        int fadeMode = 0;
    };

    // ✅ هيكل بيانات التدرج اللوني
    struct GradientElement : BaseElement {
        float r1 = 0, g1 = 0, b1 = 0, a1 = 0;
        float r2 = 0, g2 = 0, b2 = 0, a2 = 0;
    };
    

    // ✅ هيكل بيانات العنصر داخل الـ Widget (صورة البوستر مثلاً)
    struct WidgetItem {
        std::string imagePath = "";
        GLuint textureId = 0;
        int poolIndex = -1;
        bool loaded = false;
        std::string text = "";
        GLuint textTexId = 0;
        float textW = 0, textH = 0;
        float textFirstLineW = 0; // NEW
        float textFirstLineH = 0; // NEW
        std::string customAlign = ""; 
        float customW = -1.0f;
        float customH = -1.0f; // ✅ جديد: ارتفاع مخصص
        float customX = -1.0f; // ✅ جديد: إحداثي X مخصص
        float customY = -1.0f; // ✅ جديد: إحداثي Y مخصص        
        uint32_t iconCodepoint = 0;
        GLuint iconTexId = 0;
        float iconW = 0, iconH = 0;
        std::string badgeText = ""; // 🚀 مدة الفيديو
        GLuint badgeTexId = 0;
        float badgeW = 0, badgeH = 0;
        // 🚀 أضف هذه المتغيرات للأفاتار
        std::string avatarPath = ""; 
        GLuint avatarTexId = 0;
        int avatarPoolIndex = -1;
        bool avatarLoaded = false;
        // 🎨 اللون المهيمن لبوستر هذا العنصر (يُنسخ من المسبح عند جهوزية النسيج)
        float domR = 0.0f, domG = 0.0f, domB = 0.0f;
        bool  domValid = false;
        // 📊 نسبة شريط التقدّم لهذا العنصر (0..100). أي قيمة سالبة = لا يُرسم الشريط إطلاقاً
        float progress = -1.0f;
    };

    // ==================================================================================
    // 🎨 استخراج اللون المهيمن من بكسلات البوستر (يعمل في خيط التحميل، بلا أي قراءة من الـ GPU)
    // هستوغرام على 16 قطاع تدرّج لوني (Hue)، الوزن = التشبّع² × السطوع،
    // مع تجاهل البكسلات الشفافة وشبه السوداء/البيضاء/الرمادية.
    // ==================================================================================
    static int glm_extract_dominant_color(const unsigned char* px, int w, int h) {
        if (!px || w <= 0 || h <= 0) return -1;

        const long total = (long)w * (long)h;
        int step = (int)(total / 4000);           // ~4000 عيّنة مهما كان حجم الصورة
        if (step < 1) step = 1;

        const int NB = 16;                        // عدد قطاعات التدرّج اللوني
        double bw[NB] = {0}, br[NB] = {0}, bg[NB] = {0}, bb[NB] = {0};
        double avgR = 0, avgG = 0, avgB = 0; long avgN = 0;

        for (long i = 0; i < total; i += step) {
            const unsigned char* q = px + (i * 4);
            if (q[3] < 32) continue;                       // شفاف
            const int r = q[0], g = q[1], b = q[2];
            avgR += r; avgG += g; avgB += b; avgN++;

            const int mx = (r > g ? (r > b ? r : b) : (g > b ? g : b));
            const int mn = (r < g ? (r < b ? r : b) : (g < b ? g : b));
            if (mx < 38 || mx > 247) continue;             // شبه أسود / شبه أبيض
            const float v = mx / 255.0f;
            const float sat = (mx == 0) ? 0.0f : (float)(mx - mn) / (float)mx;
            if (sat < 0.18f) continue;                     // رمادي

            // زاوية التدرّج اللوني تقريبياً بلا تحويل كامل إلى HSV
            float hue;
            const float d = (float)(mx - mn);
            if (mx == r)      hue = fmod(((g - b) / d) + 6.0f, 6.0f);
            else if (mx == g) hue = ((b - r) / d) + 2.0f;
            else              hue = ((r - g) / d) + 4.0f;
            int idx = (int)(hue * (NB / 6.0f));
            if (idx < 0) idx = 0; if (idx >= NB) idx = NB - 1;

            const double wgt = (double)sat * sat * v;      // نُرجّح الألوان الحيّة
            bw[idx] += wgt; br[idx] += r * wgt; bg[idx] += g * wgt; bb[idx] += b * wgt;
        }

        int best = -1; double bestW = 0.0;
        for (int i = 0; i < NB; i++) if (bw[i] > bestW) { bestW = bw[i]; best = i; }

        double outR, outG, outB;
        if (best >= 0 && bestW > 0.0) {
            outR = br[best] / bestW; outG = bg[best] / bestW; outB = bb[best] / bestW;
        } else if (avgN > 0) {                             // بوستر رمادي/أبيض/أسود بالكامل
            outR = avgR / avgN; outG = avgG / avgN; outB = avgB / avgN;
        } else {
            return -1;
        }

        // رفع التشبّع قليلاً وتطبيع السطوع لكي لا يخرج لون باهت أو محروق
        double mx = (outR > outG ? (outR > outB ? outR : outB) : (outG > outB ? outG : outB));
        if (mx > 1.0) {
            const double target = 210.0;                   // سطوع مرجعي موحّد لكل البوسترات
            const double k = target / mx;
            outR *= k; outG *= k; outB *= k;
        }
        int R = (int)(outR + 0.5), G = (int)(outG + 0.5), B = (int)(outB + 0.5);
        if (R < 0) R = 0; if (R > 255) R = 255;
        if (G < 0) G = 0; if (G > 255) G = 255;
        if (B < 0) B = 0; if (B > 255) B = 255;
        return (R << 16) | (G << 8) | B;
    }

    // ✅ هيكل مسبح الصور (Texture Pool)
    const int MAX_TEXTURE_POOL = 300; // زيادة المسبح لمنع تجاهل الصور
    struct TexturePoolItem {
        GLuint textureId = 0;
        std::atomic<bool> inUse{false}; // 🚀 إصلاح: جعلناه ذرياً لحماية المسارات
        int width = 0;
        int height = 0;
        std::string pathToLoad = "";
        std::atomic<bool> isLoading{false};
        std::atomic<bool> isProcessing{false}; // 🚀 تمنع العمال الآخرين من أخذ نفس الصورة
        std::atomic<bool> dataReady{false};
        unsigned char* pixelData = nullptr;
        std::atomic<int> tempW{0};
        std::atomic<int> tempH{0};
        int priority = 1000; // 🚀 السحر هنا: الأولوية (0 تعني المركز وتُحمل فوراً)
        bool hasReflection = false;
        bool hasAlpha = false; // 🚀 لمعرفة هل يجب تفعيل Blend أم لا أثناء رسم البوستر
        // 🚀 Rounded corners مسبقة التجهيز داخل Texture نفسها بدل shader أثناء الحركة.
        bool preRounded = false;
        bool fitCover    = false;   // 🖼️ قصّ مركزي عند التحميل (itemImageFit="cover")
        float cornerRadius = 0.0f;
        float targetW = 0.0f;
        float targetH = 0.0f;
        std::atomic<int> domColor{-1};   // 🎨 اللون المهيمن (0xRRGGBB)، -1 = غير محسوب
    };
 

    // ✅ هيكل بيانات الـ Widget (القوائم)
    struct WidgetElement : BaseElement {
        std::string name = "";
        std::string itemType = "rect"; // ✅ نوع العنصر: rect أو circle
        bool useItemColor = false;
        float itemBgR=0.0f, itemBgG=0.0f, itemBgB=0.0f, itemBgA=1.0f;
        float itemBgR2=0.0f, itemBgG2=0.0f, itemBgB2=0.0f, itemBgA2=1.0f; // 🚀 لون التدرج الثاني لخلفية العنصر
        int itemBgGradientMode = 0; // 🚀 وضع التدرج (0=عادي, 1=عمودي, 2=أفقي)
        float fgR=1.0f, fgG=1.0f, fgB=1.0f; // foregroundColor
        float fgSelR=1.0f, fgSelG=1.0f, fgSelB=1.0f; // foregroundColorSelected
        float bgR=0.0f, bgG=0.0f, bgB=0.0f, bgA=0.0f; // backgroundColor
        int gridColumns = 10; // عدد الأعمدة الافتراضي للشبكة
        // ✅ خصائص الإطار الخاصة بحاوية الـ Widget الرئيسية (جديد)
        bool hasBorder = false;
        float borderWidth = 2.0f;
        float borderR = 1.0f, borderG = 0.0f, borderB = 0.0f, borderA = 1.0f;
        float borderR2 = 1.0f, borderG2 = 0.0f, borderB2 = 0.0f, borderA2 = 1.0f; // 🚀 لون التدرج الثاني للإطار
        int borderGradientMode = 0; // 🚀 وضع التدرج للإطار (0=عادي, 1=عمودي, 2=أفقي)
        float cornerRadius = 0.0f;
        // 👇 إضافة إعدادات خط المربعات
        std::string fontPath = "";
        int fontSize = 30;
        float textR = 1.0f, textG = 1.0f, textB = 1.0f;
        float textOffsetY = 0.0f; // للتحكم بموقع النص داخل المربع (أعلى، وسط، أسفل)
        float itemTextOffsetX = 20.0f; // إزاحة أفقية افتراضية للنص (فراغ من الحافة)
        std::string itemTextAlign = "center"; // center, left, right
        // خصائص الأيقونات داخل القوائم
        std::string iconFontPath = "";
        int iconFontSize = 30;
        float iconR = 1.0f, iconG = 1.0f, iconB = 1.0f;
        float iconSelR = 1.0f, iconSelG = 0.0f, iconSelB = 0.0f;
        float iconOffsetX = 0.0f, iconOffsetY = 0.0f;
        std::string selectionPixmapPath = "";
        bool selectionIsColor = false; // ✅ هل التحديد هو لون أم صورة؟
        bool selectionIsBorder = false; // ✅ هل التحديد هو إطار مفرغ؟
        float selBorderSize = 2.0f; // حجم الإطار
        float selBorderR=1.0f, selBorderG=0.0f, selBorderB=0.0f, selBorderA=1.0f; // لون الإطار
        float selBorderR2=1.0f, selBorderG2=0.0f, selBorderB2=0.0f, selBorderA2=1.0f; // لون التدرج الثاني للإطار
        int selBorderGradientMode = 0; // ✅ وضع التدرج (0=لا يوجد، 1=عمودي)

        float selR=0.0f, selG=0.0f, selB=0.0f, selA=0.0f; // ✅ لون التحديد (إذا كان لوناً)
        float selR2=0.0f, selG2=0.0f, selB2=0.0f, selA2=0.0f; // ✅ اللون الثاني للتدرج
        int selGradientMode = 0; // ✅ وضع التدرج لخلفية الفوكس
        GLuint selectionTexId = 0;
        float selTexW = 0, selTexH = 0;
        bool selectionLoaded = false;
        float selectionCornerRadius = 0.0f; // ✅ جديد: دوران حواف التحديد
        float itemCornerRadius = 0.0f;     // ✅ جديد: دوران حواف العناصر داخل القائمة
        float selectionPadding = -1.0f;    // ✅ جديد: للتحكم بمسافة التحديد من XML (-1 تعني أوتوماتيكي)
        bool selectionpixmapAnim = false;  // ✅ جديد: تفعيل أو تعطيل انزلاق التحديد الناعم

        // ==========================================================================
        // 📊 شريط التقدّم المدمج داخل كل عنصر (Native Progress Bar)
        //   progressBar="1"  progRect="x,y,w,h"  progCornerDia="0"
        //   progTrackColor="#3A3A42"  progFillColor="#FFFFFF"
        //   (اختياري) progTrackColorSelected / progFillColorSelected
        //   الإحداثيات x,y نسبية لصندوق العنصر نفسه (مثل itemtextoffset)
        // ==========================================================================
        // ==========================================================================
        // 📏 شريط التمرير (Scrollbar + Slider)
        //   scrollbar="ShowNever|ShowOnDemand|ShowAlways"
        //   scrollbarSize="w,h"  scrollbarOffset="x,y"  scrollbarColor  scrollbarCornerRadius
        //   sliderSize="w,h"     sliderColor            sliderCornerRadius
        //   sliderAnim="1"  = انزلاق ناعم للسلايدر + ظهور/اختفاء بالتلاشي
        // ==========================================================================
        int   scrollbarMode = 0;                 // 0=Never (الافتراضي) 1=OnDemand 2=Always
        float scrollbarW = -1.0f, scrollbarH = -1.0f;   // سالب = تلقائي
        float scrollbarOffX = 0.0f, scrollbarOffY = 0.0f;
        float scrollbarRadius = 0.0f;
        float scrollbarR = 0.16f, scrollbarG = 0.16f, scrollbarB = 0.19f, scrollbarA = 1.0f;
        float sliderW = -1.0f, sliderH = -1.0f;         // سالب = تلقائي (تناسبي)
        float sliderRadius = 0.0f;
        float sliderR = 1.0f, sliderG = 1.0f, sliderB = 1.0f, sliderA = 1.0f;
        bool  sliderAnim = true;
        // حالة داخلية (لا تُقرأ من الـ XML)
        float sliderVisualPos = -1.0f;           // 0..1 الموضع المعروض فعلياً
        float scrollbarAlpha  = 0.0f;            // شفافية التلاشي الحالية
        float scrollbarLastMoveTime = -1.0f;     // -1 = أعِد ختم الوقت في الإطار القادم

        bool  progressEnabled  = false;
        float progX = 0.0f, progY = 0.0f, progW = 0.0f, progH = 0.0f;
        float progCornerRadius = 0.0f;
        float progTrackR = 0.23f, progTrackG = 0.23f, progTrackB = 0.26f, progTrackA = 1.0f;
        float progFillR  = 1.0f,  progFillG  = 1.0f,  progFillB  = 1.0f,  progFillA  = 1.0f;
        // -1 في المكوّن الأحمر = لا يوجد لون خاص بالتحديد، استعمل اللون العادي
        float progTrackSelR = -1.0f, progTrackSelG = 0.0f, progTrackSelB = 0.0f, progTrackSelA = 1.0f;
        float progFillSelR  = -1.0f, progFillSelG  = 0.0f, progFillSelB  = 0.0f, progFillSelA  = 1.0f;
        float itemGap = 30.0f;             // ✅ جديد: التحكم في المسافة بين العناصر من XML
        bool showSelection = false; // ✅ تعديل: إيقاف التحديد الافتراضي للنصوص الثابتة
        bool hasReflection = false;
        int scrollTextMode = 0;            // ✅ جديد: نوع السكرول (0 ثابت، 1 أفقي، 2 عمودي)
        float textScrollStartTime = -1.0f; // ✅ جديد: وقت بدء التمرير
        bool textScrollActive = false;     // ✅ جديد: حالة التمرير الحالية
        bool textPending = false;          // 🚀 بقيت نصوص لم تُولَّد بعد (تُستكمل في الإطارات التالية)
        int scrollTextShot = 2;            // 🚀 عدد مرات التكرار الافتراضي (دورتين)
        int scrollTextStyle = 1;           // 🚀 نمط الحركة: 1 عادية، 2 مرتدة ذهاب وعودة
        float scrollDelay = -1.0f;         // ✅ جديد: مدة الانتظار قبل بدء السكرول

        // ✅ جديد: ظل/حدّ للنص لضمان وضوحه فوق أي خلفية (فوكس ملوّن، بوستر، تدرّج...)
        int   textShadowMode = 0;          // 0=بلا، 1=ظل مائل، 2=حدّ كامل حول الحروف
        float textShadowR = 0.0f, textShadowG = 0.0f, textShadowB = 0.0f, textShadowA = 0.75f;
        float textShadowOffX = 1.0f, textShadowOffY = 1.0f;

        float itemTextMaxW = -1.0f;        // ✅ جديد: تحديد أقصى عرض للنص لتمكين السكرول
        float selectionW = -1.0f;          // ✅ جديد: عرض مخصص للتحديد
        float selectionH = -1.0f;          // ✅ جديد: ارتفاع مخصص للتحديد
        float selectionOffsetX = 0.0f;     // ✅ جديد: إزاحة التحديد أفقياً
        float selectionOffsetY = 0.0f;     // ✅ جديد: إزاحة التحديد عمودياً

        float itemW = 220.0f; // ✅ العرض الافتراضي
        float itemH = 320.0f; // ✅ الارتفاع الافتراضي
        float itemOffsetX = 0.0f; // ✅ إزاحة يدوية أفقية للعناصر
        float itemOffsetY = 0.0f; // ✅ إزاحة يدوية عمودية للعناصر
        std::string anim = ""; // ✅ جديد: أنيميشن خاص بهذا الـ Widget
        int carouselLimit = 0; // 🚀 جديد: 0 = يتوقف في النهاية، 1 = يستمر حتى البداية

        // 🚀 أنيميشن مستقل مخصص للويدجت (لتشغيل الأعداد والنصوص متى أردنا)
        bool localAnimActive = false;
        float localAnimStartTime = -1.0f;
        std::string localAnimType = "";
        float localAnimDuration = 0.3f;
        float localAnimDistance = 20.0f;

        // ✅ تتبع موضع التمرير الفعلي لمنع القفزات
        float currentScrollOffset = 0.0f;
        float startScrollOffset = 0.0f;
        // 🚀 Prime-style carousel: المؤشر البصري يتحرك باستمرار نحو selectedIndex
        // ولا يعيد تشغيل الأنيميشن من الصفر مع كل ضغطة.
        float visualIndex = 0.0f;
        float zoomVisualIndex = 0.0f;

        std::string orientation = "vertical"; // vertical, horizontal, grid
        // 🏷️ نسخ رقمية تُحدَّث في بداية كل إطار
        OrientId    orientId  = OrientId::Vertical;
        ItemShapeId itemShape = ItemShapeId::Rect;
        int selectedIndex = 0; // فهرس العنصر المحدد حالياً
        int gradientMode = 0; // 0=none, 1=vertical, 2=horizontal
        float bgR2 = 0.0f, bgG2 = 0.0f, bgB2 = 0.0f, bgA2 = 0.0f; // اللون الثاني للتدرج
        std::vector<WidgetItem> items; // ✅ قائمة العناصر (البوسترات/الصور)

        // ✅ خصائص أنيميشن الـ Carousel والـ Zoom
        float zoomFactor = 1.2f; // مقدار التكبير للعنصر المحدد (1.2 = 120%)
        float carouselDuration = 0.4f;
        float selectionDuration = -1.0f;
        float zoomDuration = 0.3f; // ✅ مدة انيميشن التكبير (بالثواني)
        // 🖼️ itemImageFit="cover": قصّ مركزي بإحداثيات النسيج بدل تمطيط الصورة
        //    (بوستر 2:3 داخل مربع يظهر مربعاً سليماً بلا انضغاط)
        bool itemFitCover = false;
        bool zoomEnabled = true; // ✅ تفعيل أو تعطيل الـ Zoom (1 أو 0)

        // ==========================================================================
        // 🔍 انتقال قصير للتكبير عند انتقال الفوكس بين الويدجتات (set_widget_zoom).
        //   zoomShift = مقدار التكبير المطبَّق (0 = بلا تكبير، 1 = التكبير الكامل).
        //   الافتراضي 1 ⇒ كل الشاشات القديمة تعمل تماماً كما كانت.
        //
        //   ⚠️ قاعدة الأداء: في الحالة المستقرة (zoomShift = 1 وبلا انتقال جارٍ)
        //      لا يُنفَّذ أي حساب و effZoomFactor = zoomFactor حرفياً، فمسار
        //      الكاروسال يبقى كما هو بلا بايت إضافي. الانتقال يعمل لحظة
        //      down/up فقط، ولا يُضرب في tZoom إطلاقاً (لا تنعيمين فوق بعضهما).
        // ==========================================================================
        float zoomShift      = 1.0f;
        float zoomShiftFrom  = 1.0f;
        float zoomShiftTo    = 1.0f;
        float zoomShiftStart = -1.0f;   // -1 = لا انتقال جارٍ
        float zoomShiftDur   = 0.15f;   // ZoomShiftSpeed بالثواني

        // ==========================================================================
        // ✨ هالة مضيئة حول إطار التحديد (Glow)
        //   glow="1"  glowopacity="0.85"  glowsize="30"  glowcolor="#RRGGBB"
        //
        //   ⚠️ قاعدة الأداء: الهالة **لا تُرسم إطلاقاً أثناء الحركة**.
        //      هي زينة حالة سكون فقط، فلا تضيف بكسلاً واحداً أثناء الكاروسال،
        //      وتظهر فوراً مع استقرار الفوكس بلا أي انتقال زمني.
        // ==========================================================================
        bool  glowEnabled  = false;
        float glowOpacity  = 0.75f;
        float glowSize     = 30.0f;
        bool  glowHasColor = false;
        float glowR = 1.0f, glowG = 1.0f, glowB = 1.0f;
        bool isAnimating = false;
        float animStartTime = -1.0f;
        int prevIndex = 0; // العنصر السابق (للتحريك منه إليه)
    };
 

    struct ImageElement : BaseElement {
        std::string name = ""; // 🚀 إضافة دعم الاسم للصور
        std::string imagePath = "";
        GLuint textureId = 0;
        bool loaded = false;
        float cornerRadius = 0.0f; // ✅ نصف قطر الزاوية الدائرية للصور
        bool hasAlpha = false; // ✅ جديد: هل الصورة تحتوي على شفافية حقيقية (مثل PNG) أم معتمة (JPG)؟

        // 🎨 Adaptive Background Color: تلوين هذه الصورة بلون بوستر العنصر المحدَّد.
        //    الـ RGB يأتي من اللون، والشفافية من قناة ألفا الخاصة بالـ PNG نفسه،
        //    فيتحول التدرّج الأسود إلى تدرّج ملوّن بنفس الشكل تماماً.
        bool  adaptiveTint   = false;
        std::string adaptiveSource = "";   // اسم الويدجت الذي نأخذ منه لون العنصر المحدَّد
        float adaptiveDim    = 0.55f;      // معامل إعتام اللون (1 = اللون كما هو)
        float adaptiveSpeed  = 0.45f;      // مدة الانتقال بين لونين بالثواني
        float adpFallR = 0.05f, adpFallG = 0.05f, adpFallB = 0.07f;   // لون احتياطي
        // حالة التشغيل (تتغيّر أثناء الرسم)
        float adpCurR = 0.0f, adpCurG = 0.0f, adpCurB = 0.0f;
        float adpFromR = 0.0f, adpFromG = 0.0f, adpFromB = 0.0f;
        float adpTgtR = -1.0f, adpTgtG = -1.0f, adpTgtB = -1.0f;
        float adpBlendStart = -1.0f;
        bool  adpInit = false;
    };
    struct LabelElement : BaseElement {
        std::string name = ""; // 🚀 إضافة دعم الاسم للنصوص
        std::string text = ""; std::string fontPath = ""; int fontSize = 30;
        float r = 1.0f, g = 1.0f, b = 1.0f; GLuint textureId = 0; float texW = 0, texH = 0;
        float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f, bgA = 0.0f;
        float cornerRadius = 0.0f; // ✅ نصف قطر الزاوية الدائرية (cornerDia / 2)
        int gradientMode = 0; // 0=none, 1=vertical, 2=horizontal
        float bgR2 = 0.0f, bgG2 = 0.0f, bgB2 = 0.0f, bgA2 = 0.0f; // اللون الثاني للتدرج
        // ✅ خصائص الأيقونة (Icon Font)
        std::string iconFontPath = "";
        int iconFontSize = 0;
        uint32_t iconCodepoint = 0;
        GLuint iconTextureId = 0;
        float iconTexW = 0, iconTexH = 0;
        
        // ✅ خصائص محاذاة وموضعة النص
        std::string textAlign = "left"; // left, center, right, block
        AlignId     alignId   = AlignId::Left;   // 🏷️ نسخة رقمية تُحدَّث كل إطار
        bool useManualTextPos = false;
        float manualTextX = 0.0f;
        float manualTextY = 0.0f;
        
        // ✅ خصائص الإطار (Border)
        bool hasBorder = false;
        float borderWidth = 2.0f;
        float borderR = 1.0f, borderG = 0.0f, borderB = 0.0f, borderA = 1.0f;
        float borderR2 = 1.0f, borderG2 = 0.0f, borderB2 = 0.0f, borderA2 = 1.0f; // 🚀 لون التدرج الثاني
        int borderGradientMode = 0; // 🚀 وضع التدرج للإطار
        float tightY = 0.0f;  // ✅ نقطة البداية الدقيقة للحروف من الأعلى
        float tightH = 0.0f;  // ✅ الارتفاع الدقيق للحروف فقط (بدون فراغات)

        // ✅ خصائص السكرول للـ Label
        int scrollTextMode = 0;            // ✅ 0=none, 1=horizontal, 2=vertical
        float textScrollStartTime = -1.0f; 
        bool textScrollActive = true;
        int scrollTextShot = 2;            // 🚀 عدد مرات التكرار لـ Label
        int scrollTextStyle = 1;           // 🚀 نمط الحركة لـ Label
        float scrollDelay = -1.0f;         // ✅ جديد: مدة الانتظار قبل بدء السكرول
        
    };
    struct ScreenData {
        float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f;
        //float bgA = 1.0f; // ✅ الشفافية (1.0 = معتم، 0.0 = شفاف)
        float bgA = 0.0f; // 🚀 التعديل: جعل خلفية المحرك شفافة افتراضياً ليظهر الفيديو من خلفها
        std::vector<ImageElement> images;
        std::vector<WidgetElement> widgets;
        std::vector<LabelElement> labels;
        std::vector<GradientElement> gradients; // ✅ تمت إضافتها هنا بشكل صحيح
        bool slideActive = false; float slideStartTime = -1.0f;
        std::string animType = "none"; // ✅ نوع الأنيميشن
        AnimId      animTypeId = AnimId::None;   // 🏷️ نسخة رقمية تُحدَّث كل إطار
        float animDuration = 0.8f;
        float animDistance = 80.0f; 
        EaseId easeId = EaseId::Quint;   // 🎚️ منحنى التنعيم لهذه الشاشة (Easing في الـ XML)
        std::string pluginBasePath = "";
        
        std::string backdropPath = ""; // ✅ تمت الإضافة: لحفظ المسار المخصص للباكدروبات من XML
        // 🎭 قناع هذه الشاشة (mask_normal) وقناع الباكدروب المخبوز فعلياً فيها.
        //    وجودهما داخل ScreenData يعني أنهما يُحفظان ويُستعادان مع الطبقة تلقائياً.
        std::string maskPath = "";      // ما يجب أن يُخبز به أي باكدروب لهذه الشاشة
        std::string bgBakedMask = "";   // ما خُبز به الباكدروب المعروض حالياً
        
        // ✅ متغيرات الخلفية الديناميكية
        ImageElement currentBg;
        ImageElement oldBg;
        bool isBgFading = false;
        int backdropZ = 0; 
        float backdropX = 0.0f, backdropY = 0.0f, backdropW = 1920.0f, backdropH = 1080.0f;

        float bgFadeStartTime = -1.0f;
        float bgFadeDuration = 0.2f;
        // ✅ متغيرات الفيد الشامل للشاشة (Screen Fade)
        int screenFadeState = 0; 
        float screenFadeStartTime = -1.0f;
        float screenFadeDuration = 0.4f;
        // ✅ متغيرات الفيد السريع المخصص للنصوص (Mask Fade)
        bool isMaskFading = false;
        float maskFadeStartTime = -1.0f;
        float maskFadeDuration = 0.4f;
        float maskX = 0, maskY = 0, maskW = 0, maskH = 0; 
    };

    // ==================================================================================
    // 3. النظام (System Globals & Init)
    // ==================================================================================
    EGLDisplay display = EGL_NO_DISPLAY; EGLSurface surface = EGL_NO_SURFACE; EGLContext context = EGL_NO_CONTEXT;
    GLuint program = 0; GLint projLoc, colorLoc, colorEndLoc, gradientModeLoc, posLoc, texCoordLoc, useTexLoc;
    // 🚀 متغيرات مكتبة Vu+ المغلقة الأصلية (libvugles2.so)
    void* g_vugles_lib = nullptr;
    int (*p_gles_open)(void) = nullptr;
    void (*p_gles_close)(void) = nullptr;
    void (*p_gles_flush)(void) = nullptr;
    void (*p_gles_state_open)(void) = nullptr; // 🚀 الدالة المسؤولة عن ربط السياق
    bool using_vugles = false;
    GLint roundLoc, rectPosLoc, boxSizeLoc, borderWidthLoc;
    // ✅ متغيرات المحرك السريع المخصص للخلفيات
    GLuint fastProgram = 0; GLint fastProjLoc, fastColorLoc, fastPosLoc, fastTexCoordLoc, fastUseTexLoc;
    // ✅ متغيرات محرك التدرج اللوني
    GLuint gradProgram = 0; GLint gradProjLoc, gradPosLoc, gradTexCoordLoc, gradColorStartLoc, gradColorEndLoc;
    // 🪟 محرك رسم اللقطات المجمّدة: النسيج مخزَّن مسبقاً بألوان premultiplied،
    //    لذلك يجب ألا نضربه في alpha مرة ثانية كما يفعل fastProgram.
    GLuint snapProgram = 0; GLint snapProjLoc, snapPosLoc, snapTexCoordLoc, snapDimLoc, snapAlphaLoc;
    ScreenData currentScreen;

    // ✅ حماية currentScreen من الـ Race Conditions
    std::mutex screenMutex;
    std::atomic<bool> anyWidgetAnimating{false};

    // ==================================================================================
    // 🪟 نظام الشاشات المتراكبة (Screen Stack) — سكرين فوق سكرين
    // ==================================================================================
    // المبدأ: currentScreen يبقى دائماً الطبقة **العليا الحيّة**، لذلك لم يتغيّر أي سطر
    // من كود الرسم أو من دوال بايثون (كلها تعمل على الطبقة العليا تلقائياً).
    // الطبقات الأدنى تُجمَّد في نسيج FBO واحد (stackSnapTex) يُرسم كأول شيء في كل إطار،
    // وكل طبقة تحتفظ بلقطة "ما تحتها" لكي يعود المشهد سليماً تماماً عند pop.
    // التكلفة: نسيج 1920x1080 RGBA (~8MB) لكل مستوى عمق، مع سقف MAX_SCREEN_STACK.
    // ==================================================================================
    struct StackedLayer {
        ScreenData data;            // حالة الشاشة كاملة (تبقى محفوظة: التحديد، السكرول، النصوص)
        GLuint     belowTex = 0;    // لقطة ما كان تحت هذه الطبقة لحظة دفعها (0 = لا شيء)
        GLuint     belowFbo = 0;
        float      belowDim = 0.0f; // التعتيم الذي كان مطبقاً على ما تحتها

        // 🔑 حالة عامة يملكها المحمّل ولا تعيش داخل ScreenData؛ بدون حفظها هنا
        //    تفقدها الشاشة السفلية نهائياً بمجرد فتح طبقة فوقها.
        //    (القناع لم يعد هنا: صار ScreenData::maskPath فيُنقل مع الطبقة تلقائياً.)
        VolumeBarStyle  savedVolumeStyle;       // شكل شريط الصوت من XML الخاص بها
        bool            savedVolumeFromPython = false;

        // 🎬 حالة نافذة الفيديو (Hardware Layer). internal_load_interface_xml_core
        //    يصفّرهما إلى false، وهو ينفَّذ داخل push، فكانت الشاشة السفلية تفقد
        //    ثقبها الشفاف نهائياً ويبقى البفر أسود معتماً فوق الفيديو بعد pop.
        bool            savedBackdropHidden = false;
        bool            savedUiHidden       = false;
    };

    std::vector<StackedLayer> screenStack;   // الطبقات المجمّدة (الأحدث في النهاية)
    GLuint stackSnapTex = 0;                 // اللقطة المدمجة لكل ما تحت currentScreen
    GLuint stackSnapFbo = 0;
    float  stackDim = 0.0f;                  // تعتيم الطبقة السفلية الحالية (0..1)
    const int MAX_SCREEN_STACK = 4;          // سقف العمق حماية للذاكرة

    // 🎯 إعادة توجيه هدف الرسم: 0 = الشاشة الحقيقية، غير ذلك = FBO التقاط لقطة
    GLuint g_render_target_fbo = 0;
    bool   g_capturing_snapshot = false;

    // 🪟 تعليق عرض الطبقة الجديدة بعد push حتى تصل أوامر الأنيميشن من بايثون.
    // بدونه قد يلتقط مسار الرسم إطاراً وسطياً تظهر فيه عناصر الطبقة في مواضعها
    // الخام من الـ XML (وغطاء التعتيم بكامل عتامته) قبل تسليح الانزلاق ⇒ رمشة.
    // التعليق مؤقّت جداً وله سقف زمني حتى لا تتجمد الشاشة إن لم يصل الأمر أبداً.
    bool  g_push_hold = false;
    float g_push_hold_start = -1.0f;
    const float GLM_PUSH_HOLD_MAX = 0.30f;

    // 🌫️ تلاشي الطبقة الخارجة عند pop (cross-fade فوق المشهد المستعاد)
    GLuint popFadeTex = 0, popFadeFbo = 0;
    float  popFadeStart = -1.0f, popFadeDuration = 0.0f;

    // 📏 عمق المكدّس بصيغة ذرّية لتقرأه بايثون بلا انتظار القفل
    std::atomic<int> g_screen_depth{0};

    bool is_any_widget_animating() {
        return anyWidgetAnimating.load(); // قراءة ذرية بدون قفل!
    }

    // ✅ دالة داخلية للتحكم في فيد الشاشة بالكامل بأمان
    void internal_set_screen_fade(int type, float duration) {
        std::lock_guard<std::mutex> lock(screenMutex);
        currentScreen.screenFadeState = type;
        currentScreen.screenFadeStartTime = -1.0f;
        if (duration > 0.0f) currentScreen.screenFadeDuration = duration;
    }



    // ✅ دالة بايثون لتشغيل المربع الأسود المتلاشي للمعلومات
    void internal_trigger_mask_fade(float x, float y, float w, float h, float duration) {
        currentScreen.maskX = x;
        currentScreen.maskY = y;
        currentScreen.maskW = w;
        currentScreen.maskH = h;
        currentScreen.maskFadeDuration = duration > 0.0f ? duration : 0.4f;
        currentScreen.maskFadeStartTime = -1.0f;
        currentScreen.isMaskFading = true;
    }

    TexturePoolItem texturePool[MAX_TEXTURE_POOL]; // ✅ تعريف المسبح

    void reset_fb() {
        int fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd >= 0) {
            struct fb_var_screeninfo vinfo;
            if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
                vinfo.yoffset = 0;
                ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
            }
            close(fb_fd);
        }
    }

    // ==============================================================================
    // 🖼️ عرض الإطار على الشاشة
    // ==============================================================================
    // كان أحد مسارات الرسم يستدعي eglSwapBuffers مباشرة بلا فحص using_vugles،
    // فيفشل صامتاً على أجهزة Vu+. الآن كل مسارات العرض تمر من هنا.
    // ==============================================================================
    inline void glm_present() {
        // 🪟 أثناء التقاط لقطة طبقة نرسم داخل FBO فقط ولا نعرض شيئاً على الشاشة
        if (g_capturing_snapshot) return;
        auto presentStart = std::chrono::high_resolution_clock::now();
        if (using_vugles) {
            if (p_gles_flush) p_gles_flush();
        } else if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE) {
            eglSwapBuffers(display, surface);
        }
        g_perf_present_us.fetch_add((int)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - presentStart).count());
        // ⚠️ مهم: libvugles2 قد تنفّذ نداءات OpenGL خاصة بها داخل gles_flush،
        //    فتتغيّر الحالة من تحتنا. نصفّر الكاش بعد كل عرض حتى يبدأ الإطار
        //    التالي بضبط صريح للبرنامج و Blend بدل الاعتماد على قيمة قديمة.
        gl_state_cache_reset();
    }

    void internal_clear_screen() {
        if (!using_vugles && display == EGL_NO_DISPLAY) return;
        invalidate_static_scene_cache();
        invalidate_layer_scene_cache();
        glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo);
        gl_state_cache_reset();
        glViewport(0, 0, 1920, 1080);
        //glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // أسود نقي
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // 🚀 التعديل: أسود شفاف 100% ليظهر الفيديو (Hardware Layer)
        glClear(GL_COLOR_BUFFER_BIT);
        glm_present();
    }

    // ✅ دالة جديدة لإعادة تشغيل الأنيميشن في أي لحظة برمجياً
    void trigger_animation() {
        std::lock_guard<std::mutex> lock(screenMutex);
        currentScreen.slideActive = true;
        
        // 🚀 السحر هنا: فحص هل الشاشة ستقوم بصعود بوسترات ثقيلة؟
        bool needsDelay = false;
        for (const auto& w : currentScreen.widgets) {
            if (w.anim == "slide_up_fade" || w.anim == "slide_up") { needsDelay = true; break; }
        }
        
        // إذا كانت تصعد، نُعطي إشارة -2.0 (نافذة تحضير)، وإلا نبدأ فوراً -1.0
        currentScreen.slideStartTime = needsDelay ? -2.0f : -1.0f;
    }

    // 🚀 أضف هذه الدالة الجديدة بالكامل هنا 👇
    void internal_trigger_global_animation(const char* animType, float duration) {
        std::lock_guard<std::mutex> lock(screenMutex);
        g_push_hold = false;   // ✅ وصلت أوامر الأنيميشن ⇒ يُسمح بعرض الطبقة
        if (animType) currentScreen.animType = animType;
        if (duration > 0.0f) currentScreen.animDuration = duration;
        currentScreen.slideActive = true;
        currentScreen.slideStartTime = -1.0f;
        // 👇 أضف هذين السطرين لإجبار المحرك على كسر تجميد الـ FBO ورسم الأنيميشن
        invalidate_static_scene_cache();
        invalidate_layer_scene_cache();
    }

    // ==================================================================================
    // نظام التحميل المتوازي (Asynchronous Texture Loading)
    // ==================================================================================
    std::vector<int> loadQueue;
    std::mutex queueMutex;
    std::atomic<bool> keepLoaderRunning{true};
    
    std::vector<std::thread> loaderThreads;

    std::condition_variable loaderCv;   // ينتظر على queueMutex

    // 🔒 حالة debounce الباكدروب — لا تُلمس إلا مع حيازة queueMutex
    // زمنية وليست عدّاد دورات: العدّاد المشترك كان يتقدّم بثلاثة أضعاف السرعة
    // لأن ثلاثة عمال يزيدونه معاً، فينكسر التأخير المقصود (200ms).
    std::string bgDebounceLastPath = "";
    std::chrono::steady_clock::time_point bgDebounceSince{};
    bool bgDebounceArmed = false;
    static const int BG_DEBOUNCE_MS = 90;   // يكفي لابتلاع التمرير السريع بلا تأخير محسوس

    // 🚀 مدير التنظيف الذكي (Smart Teardown Manager)
    // تم نقله هنا ليرى جميع المتغيرات العامة المطلوبة
    struct EngineTeardownManager {
        ~EngineTeardownManager() {
            engine_exiting    = true;
            engine_running    = false;
            keepLoaderRunning = false;
            // إيقاظ المسارات النائمة، وإلا انتظر الخروج انتهاء المهلة
            renderCv.notify_all();
            loaderCv.notify_all();
            if (render_thread.joinable()) {
                render_thread.join();
            }
            for (auto& t : loaderThreads) {
                if (t.joinable()) t.join();
            }
            loaderThreads.clear();
        }
    };
    static EngineTeardownManager g_teardown_manager;
    

    void apply_rounded_corners_to_pixels(unsigned char* data, int imgW, int imgH, float radiusScreen, float targetW, float targetH) {
        if (!data || imgW <= 0 || imgH <= 0 || radiusScreen <= 0.0f || targetW <= 0.0f || targetH <= 0.0f) return;

        // نحول radius من إحداثيات الشاشة إلى إحداثيات الـ texture بعد التصغير.
        float rx = radiusScreen * ((float)imgW / targetW);
        float ry = radiusScreen * ((float)imgH / targetH);
        float r = std::min(rx, ry);
        if (r <= 0.5f) return;
        if (r > imgW * 0.5f) r = imgW * 0.5f;
        if (r > imgH * 0.5f) r = imgH * 0.5f;

        for (int y = 0; y < imgH; y++) {
            for (int x = 0; x < imgW; x++) {
                float cx = -1.0f;
                float cy = -1.0f;

                if (x < r && y < r) { cx = r; cy = r; }
                else if (x >= imgW - r && y < r) { cx = imgW - 1 - r; cy = r; }
                else if (x < r && y >= imgH - r) { cx = r; cy = imgH - 1 - r; }
                else if (x >= imgW - r && y >= imgH - r) { cx = imgW - 1 - r; cy = imgH - 1 - r; }
                else { continue; }

                float dx = (float)x - cx;
                float dy = (float)y - cy;
                float dist = sqrtf(dx * dx + dy * dy);
                float coverage = r - dist;

                int idx = (y * imgW + x) * 4 + 3;
                if (coverage <= 0.0f) {
                    data[idx] = 0;
                } else if (coverage < 1.0f) {
                    data[idx] = (unsigned char)((float)data[idx] * coverage);
                }
            }
        }
    }

    // ==============================================================================
    // كاش قناع الباكدروب
    // ==============================================================================
    // ⚠️ كان هذا الكاش متغيرات static داخل الدالة، والدالة تُستدعى من 3 عمال متزامنين:
    //    عامل يحرر cachedMask بينما آخر يقرأه = use-after-free وانهيار عشوائي.
    //    الآن كل الوصول محمي بقفل مخصص، ويوجد محرر صريح يُستدعى عند الإغلاق.
    // 🔒 ترتيب الأقفال: يُؤخذ maskCacheMutex آخر شيء دائماً.
    // ==============================================================================
    std::mutex maskCacheMutex;
    unsigned char* g_cachedMask = nullptr;
    int         g_cachedMaskW = 0;
    int         g_cachedMaskH = 0;
    std::string g_cachedMaskPath = "";

    void release_mask_cache() {
        std::lock_guard<std::mutex> lk(maskCacheMutex);
        if (g_cachedMask) { free(g_cachedMask); g_cachedMask = nullptr; }
        g_cachedMaskW = 0;
        g_cachedMaskH = 0;
        g_cachedMaskPath.clear();
    }

    bool composite_mask_into_backdrop_pixels(unsigned char* bg, int bw, int bh, const std::string& maskPath) {
        if (!bg || bw <= 0 || bh <= 0 || maskPath.empty()) return true;

        // القفل يغطي بناء الكاش واستهلاكه معاً — لا يمكن تحريره من تحتنا أثناء الدمج
        std::lock_guard<std::mutex> lk(maskCacheMutex);

        if (g_cachedMaskPath != maskPath || g_cachedMaskW != bw || g_cachedMaskH != bh) {
            if (g_cachedMask) {
                free(g_cachedMask);
                g_cachedMask = nullptr;
            }
            g_cachedMaskW = 0;
            g_cachedMaskH = 0;
            g_cachedMaskPath.clear();

            int mw = 0, mh = 0, mc = 0;
            unsigned char* maskOrig = stbi_load(maskPath.c_str(), &mw, &mh, &mc, 4);
            if (maskOrig) {
                // حماية من الطفح الحسابي قبل الحجز
                const size_t need = (size_t)bw * (size_t)bh * 4u;
                unsigned char* buf = (need > 0 && need / 4u / (size_t)bh == (size_t)bw)
                                     ? (unsigned char*)malloc(need) : nullptr;
                if (buf) {
                    if (mw != bw || mh != bh) {
                        // تصغير Nearest Neighbor سريع
                        float x_ratio = ((float)mw) / bw;
                        float y_ratio = ((float)mh) / bh;
                        bool aborted = false;
                        for (int i = 0; i < bh && !aborted; i++) {
                            // ✅ الإجهاض الفوري باقٍ، لكن بلا sleep داخل الحلقة:
                            //    العامل يعمل بأولوية منخفضة أصلاً (انظر image_loader_worker).
                            // الإجهاض عند وصول طلب أحدث فقط — الحركة وحدها لم تعد سبباً
                            if ((i % 64) == 0 && bgTaskPending.load()) {
                                aborted = true;
                                break;
                            }
                            int py = (int)(i * y_ratio);
                            if (py >= mh) py = mh - 1;
                            for (int j = 0; j < bw; j++) {
                                int px = (int)(j * x_ratio);
                                if (px >= mw) px = mw - 1;
                                int src_idx = (py * mw + px) * 4;
                                int dst_idx = (i * bw + j) * 4;
                                buf[dst_idx]     = maskOrig[src_idx];
                                buf[dst_idx + 1] = maskOrig[src_idx + 1];
                                buf[dst_idx + 2] = maskOrig[src_idx + 2];
                                buf[dst_idx + 3] = maskOrig[src_idx + 3];
                            }
                        }
                        if (aborted) {
                            free(buf);
                            stbi_image_free(maskOrig);
                            return false;   // الكاش يبقى فارغاً ونظيفاً
                        }
                    } else {
                        memcpy(buf, maskOrig, need);
                    }
                    g_cachedMask     = buf;
                    g_cachedMaskW    = bw;
                    g_cachedMaskH    = bh;
                    g_cachedMaskPath = maskPath;
                }
                stbi_image_free(maskOrig);
            }
        }

        if (!g_cachedMask) return true;

        const int total = bw * bh;
        for (int i = 0; i < total; i++) {
            // فحص الإجهاض كل 16384 بكسل — بلا نوم، فقط خروج مبكر
            if ((i & 0x3FFF) == 0 && bgTaskPending.load()) {
                return false;
            }

            int idx = i * 4;
            int ma = g_cachedMask[idx + 3];
            if (ma == 0) continue;

            if (ma == 255) {
                bg[idx]     = g_cachedMask[idx];
                bg[idx + 1] = g_cachedMask[idx + 1];
                bg[idx + 2] = g_cachedMask[idx + 2];
            } else {
                int inv = 255 - ma;
                bg[idx]     = (unsigned char)((bg[idx]     * inv + g_cachedMask[idx]     * ma) / 255);
                bg[idx + 1] = (unsigned char)((bg[idx + 1] * inv + g_cachedMask[idx + 1] * ma) / 255);
                bg[idx + 2] = (unsigned char)((bg[idx + 2] * inv + g_cachedMask[idx + 2] * ma) / 255);
            }
            bg[idx + 3] = 255;
        }
        return true;
    }

    // ==============================================================================
    // 🧵 عمال التحميل (Asynchronous Loaders)
    // ==============================================================================
    // ما تغيّر مقارنة بالنسخة السابقة:
    //  1. أولوية المسار تُخفَّض مرة واحدة عند البدء، بدل sleep_for(1ms) داخل حلقات
    //     البكسل (كانت تضيف ~126ms نوماً صافياً لصورة 1080p).
    //  2. الانتظار على condition_variable بدل استطلاع كل 20ms
    //     (كان 150 استيقاظاً في الثانية لثلاثة عمال على شاشة ساكنة).
    //  3. حالة debounce الباكدروب أصبحت مشتركة ومحمية بـ queueMutex،
    //     بعد أن كانت static داخل الدالة يتسابق عليها ثلاثة عمال.
    //  4. كل بيانات المهمة تُنسخ داخل القفل (LoadJob) قبل أي عمل طويل،
    //     بدل قراءة std::string من المسبح بلا حماية.
    //  5. مقبض CURL واحد لكل عامل يُعاد استخدامه (توفير مصافحة TLS في كل بوستر).
    // ==============================================================================
    struct LoadJob {
        int         idx           = -1;
        std::string path;
        bool        hasReflection = false;
        bool        preRounded    = false;
        bool        fitCover      = false;
        float       cornerRadius  = 0.0f;
        float       targetW       = 0.0f;
        float       targetH       = 0.0f;
    };

    // خفض أولوية العامل: مسار الرسم يجب أن يسبقه دائماً
    void glm_lower_worker_priority() {
        struct sched_param sp;
        std::memset(&sp, 0, sizeof(sp));
        sp.sched_priority = 0;
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
        errno = 0;
        if (nice(10) == -1 && errno != 0) {
            GLM_LOG("[GLM] worker nice() failed: %s\n", std::strerror(errno));
        }
    }

    // إعداد موحّد لمقبض CURL معاد الاستخدام
    void glm_setup_curl(CURL* curl, const char* url, long timeoutSec,
                        size_t (*cb)(void*, size_t, size_t, void*), void* userData) {
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, userData);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);   // يمنع إشارة Alarm clock
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    // كتابة ذرّية للملف: نكتب في ملف مؤقت ثم نعيد تسميته،
    // حتى لا يبقى في الكاش ملف نصف مكتوب إذا انقطعت الكهرباء أو أُغلق البلوجين.
    void glm_write_cache_file(const std::string& finalPath, const std::string& data) {
        if (data.empty()) return;
        std::string tmp = finalPath + ".part";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return;
        size_t written = fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        if (written == data.size()) rename(tmp.c_str(), finalPath.c_str());
        else unlink(tmp.c_str());
    }

    void image_loader_worker() {
        glm_lower_worker_priority();

        CURL* curl = curl_easy_init();   // مقبض واحد لعمر العامل كله

        while (keepLoaderRunning.load()) {
            // ------------------------------------------------------------------
            // 1. الانتظار حتى يصل عمل فعلي (بلا استطلاع)
            // ------------------------------------------------------------------
            {
                std::unique_lock<std::mutex> lk(queueMutex);
                loaderCv.wait_for(lk, std::chrono::milliseconds(250), [] {
                    return !keepLoaderRunning.load() || bgTaskPending.load() || !loadQueue.empty();
                });
            }
            if (!keepLoaderRunning.load()) break;

            // ------------------------------------------------------------------
            // 2. مهمة الباكدروب مع debounce مشترك
            // ------------------------------------------------------------------
            std::string localBgPath  = "";
            std::string localMaskPath = "";
            bool hasBgTask = false;

            if (bgTaskPending.load()) {
                // ==============================================================
                // Debounce قائم على **ثبات المسار** فقط، لا على توقف الحركة.
                // ==============================================================
                // سابقاً كان أي أنيميشن يعيد تصفير المؤقت، فلا يبدأ فك الضغط
                // إلا بعد توقف الكاروسال تماماً، ثم يحتاج 200ms إضافية، ثم زمن
                // فك الضغط ودمج القناع — فتظهر الصورة متأخرة جداً.
                //
                // ثبات المسار وحده يكفي للحماية من التمرير السريع: ما دام
                // المستخدم يتنقل فالمسار يتغيّر فيُعاد تصفير المؤقت تلقائياً.
                // وحين يستقر على عنصر واحد نبدأ فوراً بالتوازي مع بقية الأنيميشن،
                // فتكون البكسلات جاهزة لحظة انتهاء الحركة.
                //
                // هذا آمن الآن لأن العامل يعمل بأولوية SCHED_OTHER + nice(10)
                // بينما مسار الرسم SCHED_FIFO، فلا يمكنه سرقة زمن الرسم.
                // ورفع الصورة إلى الـ GPU يبقى مؤجَّلاً في process_ready_textures
                // حتى تنتهي الحركة، وهو ما يحمي نعومة الكاروسال فعلياً.
                const auto nowTp = std::chrono::steady_clock::now();

                bool waitMore = false;
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    if (!bgDebounceArmed || bgTaskPath != bgDebounceLastPath) {
                        bgDebounceLastPath = bgTaskPath;
                        bgDebounceSince    = nowTp;
                        bgDebounceArmed    = true;
                    }
                    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            nowTp - bgDebounceSince).count();
                    if (waited < BG_DEBOUNCE_MS) {
                        waitMore = true;
                    } else if (bgTaskPending.load()) {
                        localBgPath     = bgTaskPath;
                        localMaskPath   = bgTaskMaskPath;   // 🎭 قناع صاحب الطلب
                        bgTaskPending   = false;
                        hasBgTask       = true;
                        bgDebounceArmed = false;
                    }
                }
                if (waitMore) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }
            }

            if (hasBgTask) {
                int w = 0, h = 0, comp = 0;
                unsigned char* orig_data = nullptr;

                if (localBgPath.rfind("http://", 0) == 0 || localBgPath.rfind("https://", 0) == 0) {
                    std::string localPath = CACHE_DIR + "bg_" +
                        localBgPath.substr(localBgPath.find_last_of('/') + 1);

                    // 1. الكاش المحلي أولاً
                    if (access(localPath.c_str(), F_OK) != -1) {
                        orig_data = stbi_load(localPath.c_str(), &w, &h, &comp, 4);
                    }

                    // 2. التحميل من الشبكة إن لزم
                    if (!orig_data && curl) {
                        std::string imgBuffer;
                        glm_setup_curl(curl, localBgPath.c_str(), 10L, BackdropWriteCallback, &imgBuffer);
                        CURLcode rc = curl_easy_perform(curl);
                        if (rc != CURLE_OK) {
                            GLM_LOG("[GLM] backdrop download failed (%s): %s\n",
                                    curl_easy_strerror(rc), localBgPath.c_str());
                        }
                        if (!imgBuffer.empty() && !bgTaskPending.load()) {
                            glm_write_cache_file(localPath, imgBuffer);
                            orig_data = stbi_load_from_memory((const stbi_uc*)imgBuffer.data(),
                                                              (int)imgBuffer.size(), &w, &h, &comp, 4);
                        }
                    }
                } else {
                    orig_data = stbi_load(localBgPath.c_str(), &w, &h, &comp, 4);
                }

                if (orig_data) {
                    // إعادة جدولة المهمة إذا تحرّك المستخدم أثناء المعالجة
                    auto requeue_and_drop = [&](unsigned char* px) {
                        stbi_image_free(px);
                        if (!bgTaskPending.load()) {
                            {
                                std::lock_guard<std::mutex> lock(queueMutex);
                                bgTaskPath     = localBgPath;
                                bgTaskMaskPath = localMaskPath;  // 🎭 لا نفقد القناع عند إعادة الجدولة
                            }
                            bgTaskPending = true;
                            loaderCv.notify_one();
                        }
                    };

                    // نلغي فقط إذا وصل طلب باكدروب **أحدث** (عمل بائت فعلاً).
                    // كان الشرط يشمل anyWidgetAnimating، فيُرمى الباكدروب المفكوك
                    // ويُعاد إلى الطابور لمجرد أن الكاروسال ما زال يتحرك — ثم يعاد
                    // فك ضغطه من الصفر بعد debounce جديد. حلقة إهدار كاملة.
                    if (bgTaskPending.load()) {
                        requeue_and_drop(orig_data);
                        continue;
                    }

                    // دمج mask_normal داخل بكسلات الباكدروب مرة واحدة
                    bool success = composite_mask_into_backdrop_pixels(orig_data, w, h, localMaskPath);

                    if (!success || bgTaskPending.load()) {
                        requeue_and_drop(orig_data);
                        continue;
                    }

                    bgTaskW.store(w);
                    bgTaskH.store(h);
                    unsigned char* old = bgTaskData.exchange(orig_data);
                    if (old) stbi_image_free(old);
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        bgTaskReadyPath = localBgPath;
                        bgTaskReadyMask = localMaskPath;   // 🎭 بأي قناع خُبزت فعلاً
                    }
                    glm_request_render();  // أيقظ مسار الرسم لرفع الباكدروب
                } else if (!localBgPath.empty()) {
                    GLM_LOG("[GLM] backdrop decode failed: %s\n", localBgPath.c_str());
                }
            }

            // ------------------------------------------------------------------
            // 3. البوسترات
            // ------------------------------------------------------------------
            // فك الضغط يجري الآن أثناء الحركة أيضاً (كان يتوقف تماماً)، لأن العامل
            // بأولوية منخفضة ولا ينافس مسار الرسم. الرفع إلى الـ GPU يبقى مؤجَّلاً
            // في process_ready_textures حتى تنتهي الحركة، فتظهر البوسترات فور
            // توقف الكاروسال بدل أن يبدأ تحميلها حينها.

            // سحب أحدث مهمة (LIFO) مع نسخ كامل لبياناتها داخل القفل
            LoadJob job;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                while (!loadQueue.empty()) {
                    int cand = loadQueue.back();
                    loadQueue.pop_back();
                    if (cand < 0 || cand >= MAX_TEXTURE_POOL) continue;
                    if (!texturePool[cand].inUse.load()) {
                        texturePool[cand].isLoading = false;
                        continue;
                    }
                    const TexturePoolItem& t = texturePool[cand];
                    job.idx           = cand;
                    job.path          = t.pathToLoad;
                    job.hasReflection = t.hasReflection;
                    job.preRounded    = t.preRounded;
                    job.fitCover      = t.fitCover;
                    job.cornerRadius  = t.cornerRadius;
                    job.targetW       = t.targetW;
                    job.targetH       = t.targetH;
                    break;
                }
            }
            if (job.idx < 0 || job.path.empty()) continue;

            const int poolIdx = job.idx;
            bool wakeRenderer = false;
            int w = 0, h = 0, comp = 0;
            unsigned char* data = nullptr;

            if (job.path.rfind("http://", 0) == 0 || job.path.rfind("https://", 0) == 0) {
                std::string localPath = url_to_filename(job.path);

                if (access(localPath.c_str(), F_OK) != -1) {
                    data = stbi_load(localPath.c_str(), &w, &h, &comp, 4);
                }

                if (!data && curl) {
                    std::string imgBuffer;
                    DownloadData dlData = { &imgBuffer, &(texturePool[poolIdx].inUse) };
                    glm_setup_curl(curl, job.path.c_str(), 5L, ImageWriteCallback, &dlData);
                    CURLcode rc = curl_easy_perform(curl);
                    if (rc != CURLE_OK) {
                        GLM_LOG("[GLM] poster download failed (%s): %s\n",
                                curl_easy_strerror(rc), job.path.c_str());
                    }
                    // فحص أخير قبل فك التشفير: قد يكون العنصر خرج من الشاشة
                    if (!imgBuffer.empty() && texturePool[poolIdx].inUse.load()) {
                        glm_write_cache_file(localPath, imgBuffer);
                        data = stbi_load_from_memory((const stbi_uc*)imgBuffer.data(),
                                                     (int)imgBuffer.size(), &w, &h, &comp, 4);
                    }
                }
            } else {
                data = stbi_load(job.path.c_str(), &w, &h, &comp, 4);
            }

            if (data) {
                bool sourceHasAlpha = (comp == 4 ||
                                       job.path.find(".png")  != std::string::npos ||
                                       job.path.find(".PNG")  != std::string::npos ||
                                       job.path.find(".webp") != std::string::npos ||
                                       job.path.find(".WEBP") != std::string::npos);

                // دمج الانعكاس داخل بكسلات الصورة
                if (job.hasReflection && w > 0 && h > 0) {
                    sourceHasAlpha = true;
                    const float gapRatio = 0.02f;
                    const float refRatio = 0.15f;
                    int gap          = (int)(h * gapRatio);
                    int ref_height   = (int)(h * refRatio);
                    int total_height = h + gap + ref_height;

                    size_t need = (size_t)w * (size_t)total_height * 4u;
                    unsigned char* new_data =
                        (total_height > 0 && need / 4u / (size_t)total_height == (size_t)w)
                        ? (unsigned char*)malloc(need) : nullptr;

                    if (new_data) {
                        memset(new_data, 0, need);
                        memcpy(new_data, data, (size_t)w * (size_t)h * 4u);

                        for (int y = 0; y < ref_height; y++) {
                            int src_y = h - 1 - y;
                            int dst_y = h + gap + y;
                            float opacity = (220.0f / 255.0f) * (1.0f - ((float)y / (float)ref_height));
                            for (int x = 0; x < w; x++) {
                                int src_idx = (src_y * w + x) * 4;
                                int dst_idx = (dst_y * w + x) * 4;
                                new_data[dst_idx]     = data[src_idx];
                                new_data[dst_idx + 1] = data[src_idx + 1];
                                new_data[dst_idx + 2] = data[src_idx + 2];
                                new_data[dst_idx + 3] = (unsigned char)(data[src_idx + 3] * opacity);
                            }
                        }
                        stbi_image_free(data);
                        data = new_data;
                        h = total_height;
                    }
                }

                // تصغير البوسترات الضخمة + احترام حد الجهاز لمقاس النسيج
                int max_W = 600;
                if (max_W > glm_max_texture_size) max_W = glm_max_texture_size;
                if (w > max_W && w > 0) {
                    int new_w = max_W;
                    int new_h = (int)((long long)h * max_W / w);
                    if (new_h < 1) new_h = 1;
                    unsigned char* resized_data = (unsigned char*)malloc((size_t)new_w * new_h * 4u);
                    if (resized_data) {
                        stbir_resize_uint8(data, w, h, 0, resized_data, new_w, new_h, 0, 4);
                        stbi_image_free(data);
                        data = resized_data;
                        w = new_w; h = new_h;
                    }
                }
                if (h > glm_max_texture_size) {
                    GLM_LOG("[GLM] texture too tall (%d > %d): %s\n", h, glm_max_texture_size, job.path.c_str());
                }

                // 🖼️ itemImageFit="cover": قصّ مركزي على مستوى البكسل، مرة واحدة هنا.
                //    ⚠️ لا يجوز فعله بإحداثيات النسيج وقت الرسم: ذلك يمنع خبز الزوايا
                //       (preRounded) فيسقط البوستر إلى شادر الزوايا الثقيل في كل إطار
                //       ⇒ كاروسال متقطّع. بالقصّ هنا تبقى نسبة الصورة مطابقة للصندوق،
                //       فتُخبز الزوايا صحيحة ويعمل المسار السريع كالمعتاد.
                if (job.fitCover && job.targetW > 0.0f && job.targetH > 0.0f && w > 0 && h > 0) {
                    float imgR = (float)w / (float)h;
                    float boxR = job.targetW / job.targetH;
                    int cx = 0, cy = 0, cw = w, ch = h;
                    if (imgR > boxR + 0.001f) {          // الصورة أعرض ⇒ نقصّ من الجانبين
                        cw = (int)((float)h * boxR + 0.5f);
                        if (cw < 1) cw = 1;
                        if (cw > w) cw = w;
                        cx = (w - cw) / 2;
                    } else if (imgR < boxR - 0.001f) {   // الصورة أطول ⇒ نقصّ من أعلى وأسفل
                        ch = (int)((float)w / boxR + 0.5f);
                        if (ch < 1) ch = 1;
                        if (ch > h) ch = h;
                        cy = (h - ch) / 2;
                    }
                    if (cw != w || ch != h) {
                        unsigned char* cropped = (unsigned char*)malloc((size_t)cw * (size_t)ch * 4u);
                        if (cropped) {
                            for (int row = 0; row < ch; ++row) {
                                std::memcpy(cropped + ((size_t)row * (size_t)cw * 4u),
                                            data + ((((size_t)(cy + row) * (size_t)w) + (size_t)cx) * 4u),
                                            (size_t)cw * 4u);
                            }
                            stbi_image_free(data);
                            data = cropped;
                            w = cw; h = ch;
                        }
                    }
                }

                if (job.preRounded) {
                    apply_rounded_corners_to_pixels(data, w, h, job.cornerRadius, job.targetW, job.targetH);
                    sourceHasAlpha = true;
                }

                // 🎨 اللون المهيمن يُحسب هنا حيث البكسلات في الذاكرة أصلاً (خارج القفل)
                const int domRGB = glm_extract_dominant_color(data, w, h);

                std::lock_guard<std::mutex> lock(queueMutex);
                if (texturePool[poolIdx].inUse.load() && texturePool[poolIdx].pathToLoad == job.path) {
                    texturePool[poolIdx].tempW.store(w);
                    texturePool[poolIdx].tempH.store(h);
                    texturePool[poolIdx].pixelData = data;
                    texturePool[poolIdx].hasAlpha  = sourceHasAlpha;
                    texturePool[poolIdx].domColor.store(domRGB);
                    texturePool[poolIdx].dataReady = true;
                    wakeRenderer = true;   // الإشعار يُرسل خارج القفل
                } else {
                    stbi_image_free(data);   // العنصر خرج من الشاشة أثناء التحميل
                }
                texturePool[poolIdx].isLoading = false;
            } else {
                // رابط ميت: نعلّم المهمة كمنتهية حتى يتوقف السپينر ولا تعلق الواجهة
                std::lock_guard<std::mutex> lock(queueMutex);
                if (texturePool[poolIdx].inUse.load() && texturePool[poolIdx].pathToLoad == job.path) {
                    texturePool[poolIdx].tempW.store(0);
                    texturePool[poolIdx].tempH.store(0);
                    texturePool[poolIdx].pixelData = nullptr;
                    texturePool[poolIdx].hasAlpha  = false;
                    texturePool[poolIdx].dataReady = true;
                    wakeRenderer = true;
                }
                texturePool[poolIdx].isLoading = false;
            }

            // 🔔 خارج القفل تماماً: لا نأخذ renderCvMutex ونحن نحمل queueMutex
            if (wakeRenderer) glm_request_render();
        }

        if (curl) curl_easy_cleanup(curl);
    }

    void internal_init_gles_system(int width, int height, bool doFbReset = true) {
        if (display != EGL_NO_DISPLAY || using_vugles) return;
        
       GLM_LOG("[GLM-DEBUG] ==========================================\n");
        GLM_LOG("[GLM-DEBUG] 1. STARTING EGL INITIALIZATION\n");
        GLM_LOG("[GLM-DEBUG] Resolution requested: %d x %d\n", width, height);

        // ملاحظة: reset_fb() هنا كانت معطّلة أصلاً (الشرط بلا أثر).
        // نبقي المعامل لأن واجهة الاستدعاء تعتمده، ونوثّق أنه غير مستخدم حالياً.
        (void)doFbReset;
        
        ensure_cache_dir();
        curl_global_init(CURL_GLOBAL_ALL);
        FT_Init_FreeType(&ft_lib); 
        GLM_LOG("[GLM-DEBUG] Libraries (CURL, FreeType) initialized.\n");

        // 🚀 محاولة تحميل مكتبة Vu+ الأصلية
        if (!g_vugles_lib) {
            GLM_LOG("[GLM-DEBUG] Attempting to load libvugles2.so (Vu+ Native)...\n");
            g_vugles_lib = dlopen("libvugles2.so", RTLD_NOW | RTLD_GLOBAL);
            if (g_vugles_lib) {
                p_gles_open = (int (*)(void))dlsym(g_vugles_lib, "_Z9gles_openv");
                p_gles_close = (void (*)(void))dlsym(g_vugles_lib, "_Z10gles_closev");
                p_gles_flush = (void (*)(void))dlsym(g_vugles_lib, "_Z10gles_flushv");
                p_gles_state_open = (void (*)(void))dlsym(g_vugles_lib, "_Z15gles_state_openv");
                GLM_LOG("[GLM-DEBUG] SUCCESS: libvugles2.so loaded! (%p)\n", g_vugles_lib);
            } else {
                GLM_LOG("[GLM-DEBUG] WARNING: libvugles2.so NOT FOUND! Falling back to standard EGL.\n");
            }
        }

        if (p_gles_open) {
#ifdef GLM_DEBUG
            // توجيه stderr إلى ملف السجل — في وضع التشخيص فقط.
            // كان يُنفَّذ دائماً ويترك stderr موجّهاً للملف حتى بعد إغلاق المحرك.
            if (!freopen(GLM_LOG_PATH, "a", stderr)) { /* غير حرج */ }
#endif
            GLM_LOG("[GLM-DEBUG] Calling p_gles_open() now... WARNING: Might freeze here!\n");
            int open_result = p_gles_open();
            GLM_LOG("[GLM-DEBUG] p_gles_open() returned: %d\n", open_result);

            if (open_result == 1) {
                using_vugles = true; 
                GLM_LOG("[GLM-DEBUG] SUCCESS: EGL Initialized via libvugles2.so!\n");
                if (p_gles_state_open) {
                    GLM_LOG("[GLM-DEBUG] Calling gles_state_open() to bind context...\n");
                    p_gles_state_open();
                }
                const char* gl_version = (const char*)glGetString(GL_VERSION);
                (void)gl_version;
                GLM_LOG("[GLM-DEBUG] OpenGL Context Check: %s\n", gl_version ? gl_version : "NULL (FAILED)");
            } else {
                GLM_LOG("[GLM-DEBUG] FATAL ERROR: p_gles_open() failed! Look above for the stderr output from the library.\n");
            }
        }
        
        // 🚀 الطريقة القياسية (لا تعمل إلا إذا فشلت مكتبة Vu+)
        if (!using_vugles) {
            display = eglGetDisplay(EGL_DEFAULT_DISPLAY); 
            if (display == EGL_NO_DISPLAY) GLM_LOG("[GLM-DEBUG] ERROR: eglGetDisplay failed!\n");
            else GLM_LOG("[GLM-DEBUG] eglGetDisplay success.\n");

            EGLint major, minor;
            if (!eglInitialize(display, &major, &minor)) GLM_LOG("[GLM-DEBUG] ERROR: eglInitialize failed! Error: 0x%04x\n", eglGetError());
            else GLM_LOG("[GLM-DEBUG] eglInitialize success. Version %d.%d\n", major, minor);

            EGLConfig config; EGLint numConfig = 0; 
            EGLint configAttribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
            
            eglChooseConfig(display, configAttribs, &config, 1, &numConfig);
            if (numConfig == 0) {
                EGLint fallbackAttribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
                eglChooseConfig(display, fallbackAttribs, &config, 1, &numConfig);
            }
            if (numConfig == 0) {
                EGLint fallbackAttribs2[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
                eglChooseConfig(display, fallbackAttribs2, &config, 1, &numConfig);
            }

            if (numConfig == 0) {
                GLM_LOG("[GLM-DEBUG] FATAL ERROR: No matching EGL configs found! Aborting.\n");
                display = EGL_NO_DISPLAY;
                return;
            }

            EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
            context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
            surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, NULL);
            if (surface == EGL_NO_SURFACE) {
                EGLint pbufferAttribs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
                surface = eglCreatePbufferSurface(display, config, pbufferAttribs);
            }

            eglMakeCurrent(display, surface, surface, context);
            eglSwapInterval(display, 1); 
        } // ✅ تم إغلاق القوس هنا بشكل صحيح!

        gl_set_blend(true);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        GLM_LOG("[GLM-DEBUG] EGL INITIALIZATION FINISHED SUCCESSFULLY\n");
        GLM_LOG("[GLM-DEBUG] ==========================================\n");
        const char* vShaderStr = 
            "precision highp float; "
            "attribute vec4 vPosition; attribute vec2 vTexCoord; "
            "varying vec2 fTexCoord; varying vec2 fPixelPos; "
            "uniform mat4 uProj; "
            "void main() { gl_Position = uProj * vPosition; fTexCoord = vTexCoord; fPixelPos = vPosition.xy; }";

        const char* fShaderStr = 
            "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
            "precision highp float;\n"
            "#else\n"
            "precision mediump float;\n"
            "#endif\n"
            "varying vec2 fTexCoord; varying vec2 fPixelPos; "
            "uniform sampler2D uTexture; uniform vec4 vColor; uniform vec4 vColorEnd; uniform float uUseTex; "
            "uniform float uRoundRadius; uniform vec2 uRectPos; uniform vec2 uBoxSize; uniform float uBorderWidth; "
            "uniform float uGradientMode; "
            "void main() { "
            "  vec2 pos = fPixelPos - uRectPos; "
            "  float alpha = 1.0; "
            "  if (uRoundRadius > 0.0) { "
            "    vec2 center = uBoxSize * 0.5; "
            "    vec2 halfSize = center - 1.0; " // 🚀 توفير مساحة 1 بكسل للتنعيم
            "    vec2 d = abs(pos - center) - (halfSize - uRoundRadius); "
            "    float dist = 0.0; "
            "    if (d.x > 0.0 && d.y > 0.0) { "
            "      dist = length(d) - uRoundRadius; "
            "    } else { "
            "      dist = max(d.x, d.y) - uRoundRadius; "
            "    } "
            "    alpha = 1.0 - smoothstep(-1.0, 1.0, dist); " // 🚀 تنعيم الحافة الخارجية بالكامل
            "    if (uBorderWidth > 0.0) { "
            "       alpha *= smoothstep(-1.0, 1.0, dist + uBorderWidth); "
            "    } "
            "  } else if (uBorderWidth > 0.0) { "
            "    vec2 center = uBoxSize * 0.5; "
            "    vec2 halfSize = center - 1.0; " // 🚀 توفير مساحة 1 بكسل للإطارات غير الدائرية
            "    vec2 d = abs(pos - center); "
            "    float min_dist = min(halfSize.x - d.x, halfSize.y - d.y); "
            "    alpha *= smoothstep(-1.0, 1.0, min_dist); " // 🚀 تنعيم الحافة الخارجية
            "    alpha *= 1.0 - smoothstep(uBorderWidth - 1.0, uBorderWidth + 1.0, min_dist); " // تنعيم الحافة الداخلية
            "  } "
            "  vec4 finalColor; "
            "  if(uUseTex > 1.5) { finalColor = vec4(vColor.rgb, texture2D(uTexture, fTexCoord).a * vColor.a); } "
            "  else if(uUseTex > 0.5) { finalColor = texture2D(uTexture, fTexCoord) * vColor; } "
            "  else { "
            "      if (uGradientMode > 0.5) { "
            "          float t = (uGradientMode > 1.5) ? (pos.x / uBoxSize.x) : (pos.y / uBoxSize.y); "
            "          finalColor = mix(vColor, vColorEnd, clamp(t, 0.0, 1.0)); "
            "      } else { finalColor = vColor; } "
            "  } "
            "  finalColor.rgb *= finalColor.a; " 
            "  finalColor *= alpha; " 
            "  if (finalColor.a < 0.01) discard; " 
            "  gl_FragColor = finalColor; "
            "}";

        glm_query_limits();
        gl_state_cache_reset();

        // ✅ البناء الآن يفحص التصريف والربط، ويحرر الشادرات بعد الانتهاء (كانت تتسرب)
        program = glm_build_program(vShaderStr, fShaderStr, "main");
        if (program == 0) {
            GLM_LOG("[GLM-DEBUG] FATAL: main shader program failed to build. Aborting init.\n");
            return;   // بلا برنامج رئيسي لا معنى لمتابعة التهيئة
        }
        projLoc = glGetUniformLocation(program, "uProj"); colorLoc = glGetUniformLocation(program, "vColor");
        colorEndLoc = glGetUniformLocation(program, "vColorEnd");
        gradientModeLoc = glGetUniformLocation(program, "uGradientMode");
        posLoc = glGetAttribLocation(program, "vPosition"); texCoordLoc = glGetAttribLocation(program, "vTexCoord");
        useTexLoc = glGetUniformLocation(program, "uUseTex");
        roundLoc = glGetUniformLocation(program, "uRoundRadius");
        rectPosLoc = glGetUniformLocation(program, "uRectPos");
        boxSizeLoc = glGetUniformLocation(program, "uBoxSize");
        borderWidthLoc = glGetUniformLocation(program, "uBorderWidth");
        // ✅ بناء المحرك السريع (Fast Shader) الخالي من الحسابات المعقدة
        const char* fastVShaderStr = 
            "precision highp float; "
            "attribute vec4 vPosition; attribute vec2 vTexCoord; varying vec2 fTexCoord; "
            "uniform mat4 uProj; void main() { gl_Position = uProj * vPosition; fTexCoord = vTexCoord; }";
        const char* fastFShaderStr = 
            "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
            "precision highp float;\n"
            "#else\n"
            "precision mediump float;\n"
            "#endif\n"
            "varying vec2 fTexCoord; "
            "uniform sampler2D uTexture; uniform vec4 vColor; uniform float uUseTex; "
            //"void main() { if(uUseTex > 0.5) { gl_FragColor = texture2D(uTexture, fTexCoord) * vColor; } else { gl_FragColor = vColor; } }";
            "void main() { vec4 c; if(uUseTex > 0.5) { c = texture2D(uTexture, fTexCoord) * vColor; } else { c = vColor; } c.rgb *= c.a; gl_FragColor = c; }";

        fastProgram = glm_build_program(fastVShaderStr, fastFShaderStr, "fast");
        if (fastProgram == 0) {
            GLM_LOG("[GLM-DEBUG] FATAL: fast shader program failed to build.\n");
            glDeleteProgram(program); program = 0;
            return;
        }
        fastProjLoc = glGetUniformLocation(fastProgram, "uProj"); fastColorLoc = glGetUniformLocation(fastProgram, "vColor");
        fastPosLoc = glGetAttribLocation(fastProgram, "vPosition"); fastTexCoordLoc = glGetAttribLocation(fastProgram, "vTexCoord");
        fastUseTexLoc = glGetUniformLocation(fastProgram, "uUseTex");

        // ✅ بناء محرك التدرج اللوني (Gradient)
        const char* gradVShaderStr = 
            "precision highp float; "
            "attribute vec4 vPosition; attribute vec2 vTexCoord; varying vec2 fTexCoord; "
            "uniform mat4 uProj; void main() { gl_Position = uProj * vPosition; fTexCoord = vTexCoord; }";
        const char* gradFShaderStr = 
            "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
            "precision highp float;\n"
            "#else\n"
            "precision mediump float;\n"
            "#endif\n"
            "varying vec2 fTexCoord; "
            "uniform vec4 uColorStart; uniform vec4 uColorEnd; "
            //"void main() { gl_FragColor = mix(uColorStart, uColorEnd, fTexCoord.y); }";
            "void main() { vec4 c = mix(uColorStart, uColorEnd, fTexCoord.y); c.rgb *= c.a; gl_FragColor = c; }";

        gradProgram = glm_build_program(gradVShaderStr, gradFShaderStr, "gradient");
        if (gradProgram == 0) {
            GLM_LOG("[GLM-DEBUG] FATAL: gradient shader program failed to build.\n");
            glDeleteProgram(program);     program = 0;
            glDeleteProgram(fastProgram); fastProgram = 0;
            return;
        }
        gradProjLoc = glGetUniformLocation(gradProgram, "uProj"); gradPosLoc = glGetAttribLocation(gradProgram, "vPosition");
        gradTexCoordLoc = glGetAttribLocation(gradProgram, "vTexCoord"); gradColorStartLoc = glGetUniformLocation(gradProgram, "uColorStart");
        gradColorEndLoc = glGetUniformLocation(gradProgram, "uColorEnd");

        // 🪟 بناء محرك اللقطات (Snapshot) الخاص بالشاشات المتراكبة
        const char* snapVShaderStr =
            "precision highp float; "
            "attribute vec4 vPosition; attribute vec2 vTexCoord; varying vec2 fTexCoord; "
            "uniform mat4 uProj; void main() { gl_Position = uProj * vPosition; fTexCoord = vTexCoord; }";
        const char* snapFShaderStr =
            "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
            "precision highp float;\n"
            "#else\n"
            "precision mediump float;\n"
            "#endif\n"
            "varying vec2 fTexCoord; uniform sampler2D uTexture; uniform float uDim; uniform float uAlpha; "
            "void main() { vec4 t = texture2D(uTexture, fTexCoord); gl_FragColor = vec4(t.rgb * uDim * uAlpha, t.a * uAlpha); }";

        snapProgram = glm_build_program(snapVShaderStr, snapFShaderStr, "snapshot");
        if (snapProgram != 0) {
            snapProjLoc     = glGetUniformLocation(snapProgram, "uProj");
            snapDimLoc      = glGetUniformLocation(snapProgram, "uDim");
            snapAlphaLoc    = glGetUniformLocation(snapProgram, "uAlpha");
            snapPosLoc      = glGetAttribLocation(snapProgram, "vPosition");
            snapTexCoordLoc = glGetAttribLocation(snapProgram, "vTexCoord");
        } else {
            GLM_LOG("[GLM-DEBUG] WARNING: snapshot shader failed; screen stacking will be disabled.\n");
        }

        // ✅ تهيئة المسبح وحجز الـ IDs مسبقاً عند إقلاع النظام
        for (int i = 0; i < MAX_TEXTURE_POOL; i++) {
            // حماية من إعادة التهيئة بلا إغلاق سليم: لا نترك ID قديماً معلّقاً
            if (texturePool[i].textureId != 0) {
                glDeleteTextures(1, &texturePool[i].textureId);
                texturePool[i].textureId = 0;
            }
            if (texturePool[i].pixelData) {
                stbi_image_free(texturePool[i].pixelData);
                texturePool[i].pixelData = nullptr;
            }
            glGenTextures(1, &texturePool[i].textureId);
            texturePool[i].inUse = false;
            texturePool[i].isLoading = false;
            texturePool[i].dataReady = false;
            texturePool[i].width = 0;
            texturePool[i].height = 0;
            texturePool[i].hasAlpha = false;
            texturePool[i].preRounded = false;
            texturePool[i].fitCover   = false;
            texturePool[i].cornerRadius = 0.0f;
            texturePool[i].targetW = 0.0f;
            texturePool[i].targetH = 0.0f;
            texturePool[i].pathToLoad.clear();
        }
        // ✅ تشغيل جيش من العمال للتحميل الموازي
        keepLoaderRunning = true;
        if (loaderThreads.empty()) {
            for (int i = 0; i < NUM_LOADER_THREADS; i++) {
                loaderThreads.push_back(std::thread(image_loader_worker));
            }
        }
    }

    // 🚀 تعريف مبدئي (Forward Declaration) لحل خطأ المترجم
    void unload_widget_item_texture(WidgetItem& item);
    // 🚀 تعريف مبدئي: نحتاجه في internal_deinit_gles_system لتجميد آخر إطار قبل الإغلاق
    void internal_render_frame(int w, int h, float currentTime);
    // 🪟 نسخة بلا قفل: تُستدعى من داخل push/pop اللذين يمسكان screenMutex أصلاً
    void internal_render_frame_locked(int w, int h, float currentTime);
    void internal_load_interface_xml_core(const char* xmlPath, const char* screenName, const char* pluginPath);
    void glm_release_screen_gpu(ScreenData& s);
    
    void internal_deinit_gles_system() {
        // ✅ 1. إيقاف جميع عمال التحميل بأمان
        keepLoaderRunning = false;
        loaderCv.notify_all();   // إيقاظ فوري بدل انتظار انتهاء مهلة الانتظار
        // 🔊 تصفير حالة شريط الصوت عند إغلاق المحرك
        volume_active = false;
        volume_show_start = -1.0f;
        volume_touch_time = -1.0f;
        volume_fill_snap = true;
        volume_last_draw_time = -1.0f;
        // 🚀 تصفير المتغيرات عند تدمير الواجهة وإغلاقها
        backdrop_hidden = false;
        ui_hidden = false;
        last_backdrop_hidden = false;
        trailer_fade_start = -1.0f;
        trailer_post_render_until = -1.0f;
        trailer_cover_visible = false;
        trailer_cover_alpha = 0.0f;
        trailer_cover_fade_start = -1.0f;
        
        for (auto& t : loaderThreads) {
            if (t.joinable()) {
                t.join();
            }
        }
        loaderThreads.clear();

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            loadQueue.clear();
        }
        // ✅ تنظيف مهمة تحميل الـ Fanart إذا كانت قيد المعالجة أثناء الخروج
        unsigned char* bData = bgTaskData.exchange(nullptr);
        if (bData) stbi_image_free(bData);
        bgTaskPending = false;

        if (using_vugles || display != EGL_NO_DISPLAY) {
            

            // ✅ 3. تنظيف الذاكرة بالكامل وتفريغ كرت الشاشة (لمنع تداخل الصور وظهور البوستر كخلفية)
            for (auto& img : currentScreen.images) {
                if (img.textureId != 0) glDeleteTextures(1, &img.textureId);
            }
            for (auto& w : currentScreen.widgets) {
                for (auto& item : w.items) {
                    unload_widget_item_texture(item); // ✅ إفراغ الخانة بأمان
                    // ⚠️ تحذير: لا تحذف item.textureId أبداً هنا!
                    // ⚠️ item.textTexId و badgeTexId يملكهما globalTextCache وحده،
                    //    وحذفهما هنا كان يعني حذفاً مزدوجاً لنفس الاسم.
                    item.textTexId  = 0;
                    item.badgeTexId = 0;
                    if (item.iconTexId != 0) glDeleteTextures(1, &item.iconTexId);
                    if (item.avatarTexId != 0) glDeleteTextures(1, &item.avatarTexId);
                }
                if (w.selectionTexId != 0) glDeleteTextures(1, &w.selectionTexId);
            }
            for (auto& lbl : currentScreen.labels) {
                lbl.textureId = 0;   // ⚠️ يملكه globalTextCache — لا يُحذف هنا
                if (lbl.iconTextureId != 0) glDeleteTextures(1, &lbl.iconTextureId);
            }

            // ✅ مسح الخلفيات الديناميكية
            if (currentScreen.currentBg.textureId != 0) glDeleteTextures(1, &currentScreen.currentBg.textureId);
            if (currentScreen.oldBg.textureId != 0) glDeleteTextures(1, &currentScreen.oldBg.textureId);

            // ✅ تصفير وإعادة تهيئة كاملة لمتغيرات الفيد والأنيميشن لمنع تداخلها مع الشاشة التالية
            currentScreen.currentBg = ImageElement();
            currentScreen.oldBg = ImageElement();
            currentScreen.isBgFading = false;
            currentScreen.bgFadeStartTime = -1.0f;
            currentScreen.isMaskFading = false;
            currentScreen.maskFadeStartTime = -1.0f;
            currentScreen.slideActive = false;
            currentScreen.slideStartTime = -1.0f;
            currentScreen.screenFadeState = 0;
            currentScreen.screenFadeStartTime = -1.0f;

            // مسح القوائم تماماً (Clear Vectors)
            currentScreen.images.clear();
            currentScreen.widgets.clear();
            currentScreen.labels.clear();

            // ✅ إضافة مسح التدرجات اللونية لمنع بقائها في الذاكرة بعد غلق البلوجين
            currentScreen.gradients.clear();

            // 🚀 تحرير كل نسج المسبح وبياناتها المعلّقة
            // (كانت 300 نسيج تتسرب في كل دورة فتح/إغلاق للبلوجين)
            for (int i = 0; i < MAX_TEXTURE_POOL; i++) {
                if (texturePool[i].textureId != 0) {
                    glDeleteTextures(1, &texturePool[i].textureId);
                    texturePool[i].textureId = 0;
                }
                if (texturePool[i].pixelData) {
                    stbi_image_free(texturePool[i].pixelData);
                    texturePool[i].pixelData = nullptr;
                }
                texturePool[i].inUse     = false;
                texturePool[i].isLoading = false;
                texturePool[i].dataReady = false;
                texturePool[i].width  = 0;
                texturePool[i].height = 0;
                texturePool[i].pathToLoad.clear();
            }

            // 🪟 تحرير كل الطبقات المتراكبة ولقطاتها قبل تدمير السياق
            for (size_t si = 0; si < screenStack.size(); ++si) {
                if (screenStack[si].belowTex != 0) glDeleteTextures(1, &screenStack[si].belowTex);
                if (screenStack[si].belowFbo != 0) glDeleteFramebuffers(1, &screenStack[si].belowFbo);
            }
            screenStack.clear();
            if (stackSnapTex != 0) { glDeleteTextures(1, &stackSnapTex); stackSnapTex = 0; }
            if (stackSnapFbo != 0) { glDeleteFramebuffers(1, &stackSnapFbo); stackSnapFbo = 0; }
            if (popFadeTex != 0)   { glDeleteTextures(1, &popFadeTex);   popFadeTex = 0; }
            if (popFadeFbo != 0)   { glDeleteFramebuffers(1, &popFadeFbo); popFadeFbo = 0; }
            popFadeStart = -1.0f; popFadeDuration = 0.0f;
            stackDim = 0.0f; g_render_target_fbo = 0; g_capturing_snapshot = false;

            // 🚀 تنظيف لقطة الـ FBO قبل تدمير سياق OpenGL
            if (staticSceneTex != 0) { glDeleteTextures(1, &staticSceneTex); staticSceneTex = 0; }
            if (staticSceneFbo != 0) { glDeleteFramebuffers(1, &staticSceneFbo); staticSceneFbo = 0; }
            staticSceneW = 0; staticSceneH = 0; staticSceneValid = false; staticSceneDirty.store(true);
            if (layerSceneTex != 0) { glDeleteTextures(1, &layerSceneTex); layerSceneTex = 0; }
            if (layerSceneFbo != 0) { glDeleteFramebuffers(1, &layerSceneFbo); layerSceneFbo = 0; }
            layerSceneW = 0; layerSceneH = 0; layerSceneSplitZ = 999999; layerSceneValid = false; layerSceneDirty.store(true);
            
            // ==============================================================
            // ⚠️ كل نداءات OpenGL يجب أن تتم قبل تدمير السياق.
            //    في النسخة السابقة كان كاش النصوص والسپينر يُحذفان *بعد*
            //    p_gles_close()/eglTerminate() — أي نداءات على سياق معدوم،
            //    فلا تُحرَّر النسج فعلياً وقد تنهار على بعض السائقين.
            // ==============================================================

            // 🚀 تفريغ كاش نصوص كرت الشاشة (الـ VRAM)
            // الكاش هو المالك الوحيد للنسج، لذلك clear() تحذفها فعلياً.
            globalTextCache.clear();

            // 🚀 تصفير نسج السپينر لمنع تداخل الـ Texture IDs
            if (texLoadingBack != 0)    { glDeleteTextures(1, &texLoadingBack);    texLoadingBack = 0; }
            if (texLoadingSpinner != 0) { glDeleteTextures(1, &texLoadingSpinner); texLoadingSpinner = 0; }

            // 🚀 حذف برامج الشادر (كانت تُصفَّر فقط بلا glDeleteProgram = تسريب)
            if (program != 0)     { glDeleteProgram(program);     program = 0; }
            if (fastProgram != 0) { glDeleteProgram(fastProgram); fastProgram = 0; }
            if (gradProgram != 0) { glDeleteProgram(gradProgram); gradProgram = 0; }
            if (snapProgram != 0) { glDeleteProgram(snapProgram); snapProgram = 0; }
            gl_state_cache_reset();

            // ✅ 4. تدمير سياق OpenGL (بعد أن حُرِّرت كل موارد الـ GPU)
            if (using_vugles && p_gles_close) {
                p_gles_close();
                using_vugles = false;
            } else {
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
                if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
                eglTerminate(display);
            }
            display = EGL_NO_DISPLAY;
            context = EGL_NO_CONTEXT;
            surface = EGL_NO_SURFACE;

            // احتياط: أي مدخلات وصلت بعد clear() تُسقَط بلا نداءات OpenGL
            globalTextCache.drop_without_gl();

            // 🚀 تحرير كاش قناع الباكدروب (كان يبقى حتى 8 ميجابايت عالقة)
            release_mask_cache();

            // 🚀 تفريغ ذاكرة الكاش للخطوط عند الخروج للحفاظ على نظافة الرام
            for (auto& fc : fontBufferCache) {
                delete[] fc.second.first;
            }
            fontBufferCache.clear();
            // 🚀 تنظيف كاش HarfBuzz و FreeType بأمان
            for (auto& pair : hb_font_cache) hb_font_destroy(pair.second);
            hb_font_cache.clear();
            for (auto& pair : ft_face_cache) FT_Done_Face(pair.second);
            ft_face_cache.clear();
            FT_Done_FreeType(ft_lib);
            ft_lib = nullptr;

            // 🚀 تنظيف مكتبة الإنترنت بأمان عند الخروج
            curl_global_cleanup();

            // إعادة الـ framebuffer إلى الوضع الطبيعي الخاص بـ Enigma2
            reset_fb();
        }
    }

    // ==================================================================================
    // 4. إنشاء خطوط النصوص (Texture Generation) بواسطة FreeType و HarfBuzz
    // ==================================================================================

    // ✅ إعادة إضافة دالة قراءة الحروف بترميز UTF-8 مع حماية صارمة ضد التجاوز (Segfault Protection)
    uint32_t next_utf8_codepoint(const unsigned char*& p) {
        if (!p || !*p) return 0;
        uint32_t cp = 0;
        if ((*p & 0x80) == 0) { 
            cp = *p++; 
        } else if ((*p & 0xE0) == 0xC0) { 
            cp = (*p++ & 0x1F) << 6; 
            if (*p) cp |= (*p++ & 0x3F); // 🚀 حماية: التأكد من عدم تجاوز نهاية النص
        } else if ((*p & 0xF0) == 0xE0) { 
            cp = (*p++ & 0x0F) << 12; 
            if (*p) cp |= (*p++ & 0x3F) << 6; 
            if (*p) cp |= (*p++ & 0x3F); 
        } else if ((*p & 0xF8) == 0xF0) { 
            cp = (*p++ & 0x07) << 18; 
            if (*p) cp |= (*p++ & 0x3F) << 12; 
            if (*p) cp |= (*p++ & 0x3F) << 6; 
            if (*p) cp |= (*p++ & 0x3F); 
        } else { 
            p++; 
        }
        return cp;
    }

    // 🚀 محرك البحث عن الخطوط البديلة (Font Fallback Engine)
    FT_Face get_font_for_char(uint32_t codepoint, FT_Face primaryFace, const std::string& primaryPath, std::string& outPath) {
        // 1. إذا كان الحرف موجوداً في الخط الأساسي (مثل Poppins)، نستخدمه فوراً
        if (FT_Get_Char_Index(primaryFace, codepoint) != 0) {
            outPath = primaryPath;
            return primaryFace;
        }

        // 2. إذا لم يكن موجوداً (عربي، رموز، إيموجي)، نبحث في بنك السرقات!
        std::vector<std::string> fallbacks = {
            currentScreen.pluginBasePath + "fonts/fallback.ttf", // 👈 خط البنك الشامل الذي ستضعه أنت
            //currentScreen.pluginBasePath + "fonts/seguiemj.ttf",     // 🚀 خط رموز ويندوز (شامل جداً للكرة والرموز)
            "/usr/share/fonts/ae_AlMateen.ttf",                  // خط Enigma2 الاحتياطي
            "/usr/share/fonts/nmsbd.ttf"                         // خط الإنيجما الكلاسيكي
        };

        for (const auto& path : fallbacks) {
            FT_Face fbFace = get_ft_face(path);
            if (fbFace && FT_Get_Char_Index(fbFace, codepoint) != 0) {
                outPath = path;
                return fbFace; // سرقنا الحرف بنجاح!
            }
        }

        // 3. إذا فشل كل شيء، نعود للأساسي (ليرسم مربع)
        outPath = primaryPath;
        return primaryFace;
    }

    void create_label_texture(LabelElement& lbl) {
        if (lbl.text.empty()) return;
        std::string fontCandidate = (!lbl.fontPath.empty() && lbl.fontPath[0] == '/')
                                    ? lbl.fontPath : currentScreen.pluginBasePath + lbl.fontPath;
        std::string cacheKey = fontCandidate + "_" + std::to_string(lbl.fontSize) + "_" + lbl.text;
        if (const CachedText* c = globalTextCache.get(cacheKey)) {
            lbl.textureId = c->texId;
            lbl.texW = (float)c->w;
            lbl.texH = (float)c->h;
            if (lbl.w <= 0) lbl.w = lbl.texW;
            if (lbl.h <= 0) lbl.h = lbl.texH;
            return;
        }

        FT_Face primaryFace = get_ft_face(fontCandidate); if (!primaryFace) return;

        struct TextSegment { std::string text; FT_Face face; std::string path; bool isArabic; };
        std::vector<TextSegment> segments;
        TextSegment currentSeg;
        currentSeg.face = primaryFace;
        currentSeg.path = fontCandidate;
        currentSeg.isArabic = false;
        bool firstChar = true;

        const unsigned char* p = (const unsigned char*)lbl.text.c_str();
        while (*p) {
            const unsigned char* start_p = p;
            uint32_t cp = next_utf8_codepoint(p);
            if (cp == 0) break;
            std::string charStr((const char*)start_p, p - start_p);

            std::string reqPath;
            FT_Face reqFace = get_font_for_char(cp, primaryFace, fontCandidate, reqPath);

            bool isNeutral = (cp == ' ' || cp == '\t' || cp == '-' || cp == '_' || cp == ':' || cp == '|' || cp == '/' || cp == '\\' || cp == '.' || cp == ',' || cp == '(' || cp == ')' || cp == '[' || cp == ']');
            bool charIsArabic = currentSeg.isArabic;
            if (!isNeutral) {
                charIsArabic = (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF);
            }

            if (firstChar) {
                currentSeg.isArabic = charIsArabic;
                firstChar = false;
            }

            if (cp == ' ' || cp == '\t') { reqFace = currentSeg.face; reqPath = currentSeg.path; }

            if ((reqFace != currentSeg.face || charIsArabic != currentSeg.isArabic) && !currentSeg.text.empty()) {
                segments.push_back(currentSeg);
                currentSeg.text = "";
                currentSeg.isArabic = charIsArabic;
            }
            currentSeg.text += charStr;
            currentSeg.face = reqFace;
            currentSeg.path = reqPath;
        }
        if (!currentSeg.text.empty()) segments.push_back(currentSeg);

        FT_Set_Pixel_Sizes(primaryFace, 0, lbl.fontSize);
        int max_asc = primaryFace->size->metrics.ascender >> 6;
        int max_desc = std::abs(primaryFace->size->metrics.descender >> 6);
        int tw = 0;

        struct SegData { hb_buffer_t* buf; hb_glyph_info_t* info; hb_glyph_position_t* pos; unsigned int count; FT_Face f; std::string text; };
        std::vector<SegData> renderData;

        for (auto& seg : segments) {
            FT_Set_Pixel_Sizes(seg.face, 0, lbl.fontSize);
            hb_font_t *hb_font = hb_font_cache[seg.path];
            hb_ft_font_changed(hb_font);

            hb_buffer_t *hb_buffer = hb_buffer_create();
            hb_buffer_add_utf8(hb_buffer, seg.text.c_str(), -1, 0, -1);
            
            if (seg.isArabic) {
                hb_buffer_set_direction(hb_buffer, HB_DIRECTION_RTL);
                hb_buffer_set_script(hb_buffer, HB_SCRIPT_ARABIC);
            } else {
                hb_buffer_set_direction(hb_buffer, HB_DIRECTION_LTR);
                hb_buffer_guess_segment_properties(hb_buffer);
            }

            hb_shape(hb_font, hb_buffer, NULL, 0);

            unsigned int count;
            hb_glyph_info_t *info = hb_buffer_get_glyph_infos(hb_buffer, &count);
            hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(hb_buffer, &count);

            SegData sd = {hb_buffer, info, pos, count, seg.face, seg.text};
            renderData.push_back(sd);

            for (unsigned int i = 0; i < count; ++i) {
                tw += pos[i].x_advance >> 6;
            }
        }

        // --- Justify (block align) logic for Label ---
        int spaceCount = std::count(lbl.text.begin(), lbl.text.end(), ' ');
        float extraSpace = 0.0f;
        if (lbl.textAlign == "block" && spaceCount > 0 && lbl.w > 0 && tw < lbl.w) {
            extraSpace = (lbl.w - tw) / (float)spaceCount;
            tw = lbl.w; // Expand texture width to fill the box
        }
        // ---------------------------------------------

        if(max_asc == 0) max_asc = primaryFace->size->metrics.ascender >> 6;
        if(max_desc == 0) max_desc = std::abs(primaryFace->size->metrics.descender >> 6);

        int h = max_asc + max_desc + 4;
        int baseline = max_asc + 2;
        // 🛡️ حماية من تجاوز حد الجهاز: نص طويل جداً كان ينتج نسيجاً يفشل صامتاً
        //    فيختفي النص كلياً بلا أي رسالة.
        if (tw > glm_max_texture_size) {
            GLM_LOG("[GLM] label texture clamped %d -> %d ('%s')\n",
                    tw, glm_max_texture_size, lbl.text.c_str());
            tw = glm_max_texture_size;
        }
        if (h > glm_max_texture_size) h = glm_max_texture_size;
        if (tw <= 0 || h <= 0) { for (auto& sd : renderData) hb_buffer_destroy(sd.buf); return; }

        unsigned char* bmp = new (std::nothrow) unsigned char[(size_t)tw * h * 4]();
        if (!bmp) { for (auto& sd : renderData) hb_buffer_destroy(sd.buf); return; }
        for (int i = 0; i < tw * h; i++) { bmp[i*4]=255; bmp[i*4+1]=255; bmp[i*4+2]=255; bmp[i*4+3]=0; }

        float x = 0.0f;
        for (auto& sd : renderData) {
            FT_Set_Pixel_Sizes(sd.f, 0, lbl.fontSize);
            std::vector<bool> addedSpace(sd.text.length(), false);
            for (unsigned int i = 0; i < sd.count; ++i) {
                FT_Load_Glyph(sd.f, sd.info[i].codepoint, FT_LOAD_RENDER);
                FT_Bitmap* ft_bmp = &sd.f->glyph->bitmap;
                int px_x = (int)x + (sd.pos[i].x_offset >> 6) + sd.f->glyph->bitmap_left;
                int px_y = baseline - (sd.pos[i].y_offset >> 6) - sd.f->glyph->bitmap_top;

                for (unsigned int r = 0; r < ft_bmp->rows; ++r) {
                    for (unsigned int c = 0; c < ft_bmp->width; ++c) {
                        int final_x = px_x + c;
                        int final_y = px_y + r;
                        if (final_x >= 0 && final_x < tw && final_y >= 0 && final_y < h) {
                            int idx = (final_y * tw + final_x) * 4;
                            bmp[idx+3] = ft_bmp->buffer[r * ft_bmp->pitch + c];
                        }
                    }
                }
                x += sd.pos[i].x_advance >> 6;
                // Add extra space for block alignment
                if (extraSpace > 0.0f && sd.info[i].cluster < sd.text.length()) {
                    if (sd.text[sd.info[i].cluster] == ' ' && !addedSpace[sd.info[i].cluster]) {
                        x += extraSpace;
                        addedSpace[sd.info[i].cluster] = true;
                    }
                }
            }
            hb_buffer_destroy(sd.buf);
        }
        
        int w = tw;
        // ⚠️ لا نحذف lbl.textureId القديم: قد يكون ما زال مسجّلاً في globalTextCache
        //    تحت مفتاح النص السابق ويستخدمه عنصر آخر. الكاش وحده يحذف نسج النصوص.
        lbl.textureId = 0;
        glGenTextures(1, &lbl.textureId);
        glBindTexture(GL_TEXTURE_2D, lbl.textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); 
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // 🚀 قفل الحواف لمنع الخطوط السفلية والعلوية
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp); delete[] bmp;
        //hb_buffer_destroy(hb_buffer);
        
        globalTextCache.put(cacheKey, CachedText{lbl.textureId, w, h, 0, 0});
        lbl.tightY = 0;
        lbl.tightH = (float)h;
        if (lbl.w <= 0) lbl.w = (float)w;
        if (lbl.h <= 0) lbl.h = (float)h;
        lbl.texW = (float)w;
        lbl.texH = (float)h;
    }

    void create_icon_texture(LabelElement& lbl) {
        if (lbl.iconFontPath.empty() || lbl.iconCodepoint == 0) return;
        std::string fontCandidate = (!lbl.iconFontPath.empty() && lbl.iconFontPath[0] == '/') ? lbl.iconFontPath : currentScreen.pluginBasePath + lbl.iconFontPath;
        FT_Face face = get_ft_face(fontCandidate); if (!face) return;
        FT_Set_Pixel_Sizes(face, 0, lbl.iconFontSize);
        FT_Load_Char(face, lbl.iconCodepoint, FT_LOAD_RENDER);
        FT_Bitmap* ft_bmp = &face->glyph->bitmap;
        int w = ft_bmp->width, h = ft_bmp->rows; if (w <= 0 || h <= 0) return;

        unsigned char* bmp = new (std::nothrow) unsigned char[(size_t)w * h * 4]();
        if (!bmp) return;
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                int idx = (r * w + c) * 4; bmp[idx]=255; bmp[idx+1]=255; bmp[idx+2]=255; bmp[idx+3]=ft_bmp->buffer[r * ft_bmp->pitch + c];
            }
        }
        if (lbl.iconTextureId != 0) glDeleteTextures(1, &lbl.iconTextureId);
        glGenTextures(1, &lbl.iconTextureId);
        glBindTexture(GL_TEXTURE_2D, lbl.iconTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); 
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // 🚀
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // 🚀
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp); delete[] bmp;
        lbl.iconTexW = w; lbl.iconTexH = h;
    }

    void create_widget_text_texture(WidgetElement& w, WidgetItem& item) {
        g_perf_texts.fetch_add(1);
        if (item.text.empty()) return;
        std::string fontCandidate = w.fontPath.empty() ? "fonts/M-Bold.ttf" : w.fontPath;
        fontCandidate = (!fontCandidate.empty() && fontCandidate[0] == '/') ? fontCandidate : currentScreen.pluginBasePath + fontCandidate;
        std::string cacheKey = fontCandidate + "_" + std::to_string(w.fontSize) + "_" + item.text;
        
        // 🚀 1. جلب البيانات من الكاش بما فيها السطر الأول
        if (const CachedText* c = globalTextCache.get(cacheKey)) {
            item.textTexId      = c->texId;
            item.textW          = (float)c->w;
            item.textH          = (float)c->h;
            item.textFirstLineW = (float)c->firstLineW;
            item.textFirstLineH = (float)c->firstLineH;
            return;
        }

        FT_Face primaryFace = get_ft_face(fontCandidate); if (!primaryFace) return;

        std::vector<std::string> stringLines;
        std::string tempText = item.text;
        size_t delPos = 0;
        while ((delPos = tempText.find("||")) != std::string::npos) {
            stringLines.push_back(tempText.substr(0, delPos));
            tempText.erase(0, delPos + 2);
        }
        stringLines.push_back(tempText);

        struct SegData { hb_buffer_t* buf; hb_glyph_info_t* info; hb_glyph_position_t* pos; unsigned int count; FT_Face f; std::string text; };
        struct LineRenderData { std::vector<SegData> segments; int tw = 0; int h = 0; int baseline = 0; int fontSize = 0; float colorDim = 1.0f; float extraSpace = 0.0f; };
        
        std::vector<LineRenderData> finalLines;
        int max_texture_w = 0;
        int total_texture_h = 0;
        int lineGap = 6; 

        for (size_t lineIdx = 0; lineIdx < stringLines.size(); lineIdx++) {
            std::string lineStr = stringLines[lineIdx];
            
            int currentFontSize = w.fontSize;
            float currentColorDim = 1.0f;
            if (lineIdx > 0) {
                currentFontSize = (int)(w.fontSize * 0.75f);
                currentColorDim = 0.65f; 
            }

            struct TextSegment { std::string text; FT_Face face; std::string path; bool isArabic; };
            std::vector<TextSegment> segments;
            TextSegment currentSeg;
            currentSeg.face = primaryFace;
            currentSeg.path = fontCandidate;
            currentSeg.isArabic = false;
            bool firstChar = true;

            const unsigned char* p = (const unsigned char*)lineStr.c_str();
            while (*p) {
                const unsigned char* start_p = p;
                uint32_t cp = next_utf8_codepoint(p);
                if (cp == 0) break;
                std::string charStr((const char*)start_p, p - start_p);

                std::string reqPath;
                FT_Face reqFace = get_font_for_char(cp, primaryFace, fontCandidate, reqPath);

                bool isNeutral = (cp == ' ' || cp == '\t' || cp == '-' || cp == '_' || cp == ':' || cp == '|' || cp == '/' || cp == '\\' || cp == '.' || cp == ',' || cp == '(' || cp == ')' || cp == '[' || cp == ']');
                bool charIsArabic = currentSeg.isArabic;
                if (!isNeutral) {
                    charIsArabic = (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF);
                }

                if (firstChar) { currentSeg.isArabic = charIsArabic; firstChar = false; }
                if (cp == ' ' || cp == '\t') { reqFace = currentSeg.face; reqPath = currentSeg.path; }

                if ((reqFace != currentSeg.face || charIsArabic != currentSeg.isArabic) && !currentSeg.text.empty()) {
                    segments.push_back(currentSeg);
                    currentSeg.text = "";
                    currentSeg.isArabic = charIsArabic;
                }
                currentSeg.text += charStr;
                currentSeg.face = reqFace;
                currentSeg.path = reqPath;
            }
            if (!currentSeg.text.empty()) segments.push_back(currentSeg);

            FT_Set_Pixel_Sizes(primaryFace, 0, currentFontSize);
            int max_asc = primaryFace->size->metrics.ascender >> 6;
            int max_desc = std::abs(primaryFace->size->metrics.descender >> 6);
            int line_tw = 0;

            LineRenderData lrd;
            lrd.fontSize = currentFontSize;
            lrd.colorDim = currentColorDim;

            for (auto& seg : segments) {
                FT_Set_Pixel_Sizes(seg.face, 0, currentFontSize);
                hb_font_t *hb_font = hb_font_cache[seg.path];
                hb_ft_font_changed(hb_font);

                hb_buffer_t *hb_buffer = hb_buffer_create();
                hb_buffer_add_utf8(hb_buffer, seg.text.c_str(), -1, 0, -1);
                
                if (seg.isArabic) {
                    hb_buffer_set_direction(hb_buffer, HB_DIRECTION_RTL);
                    hb_buffer_set_script(hb_buffer, HB_SCRIPT_ARABIC);
                } else {
                    hb_buffer_set_direction(hb_buffer, HB_DIRECTION_LTR);
                    hb_buffer_guess_segment_properties(hb_buffer);
                }
                hb_shape(hb_font, hb_buffer, NULL, 0);

                unsigned int count;
                hb_glyph_info_t *info = hb_buffer_get_glyph_infos(hb_buffer, &count);
                hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(hb_buffer, &count);

                SegData sd = {hb_buffer, info, pos, count, seg.face, seg.text};
                lrd.segments.push_back(sd);

                for (unsigned int i = 0; i < count; ++i) {
                    line_tw += pos[i].x_advance >> 6;
                }
            }

            std::string align = (!item.customAlign.empty()) ? item.customAlign : w.itemTextAlign;
            int spaceCount = std::count(lineStr.begin(), lineStr.end(), ' ');
            float currentItemW = (item.customW > 0.0f) ? item.customW : w.itemW;
            float maxTxtW = currentItemW - (w.itemTextOffsetX * 2.0f);
            if (w.itemTextMaxW > 0.0f) maxTxtW = w.itemTextMaxW;
            if (maxTxtW <= 0.0f) maxTxtW = currentItemW;

            if (align == "block" && spaceCount > 0 && line_tw < maxTxtW) {
                lrd.extraSpace = (maxTxtW - line_tw) / (float)spaceCount;
                line_tw = maxTxtW; 
            }

            if(max_asc == 0) max_asc = primaryFace->size->metrics.ascender >> 6;
            if(max_desc == 0) max_desc = std::abs(primaryFace->size->metrics.descender >> 6);

            lrd.tw = line_tw;
            lrd.h = max_asc + max_desc + 4; 
            lrd.baseline = max_asc + 2;

            if (line_tw > max_texture_w) max_texture_w = line_tw;
            
            finalLines.push_back(lrd);
            total_texture_h += lrd.h;
            if (lineIdx < stringLines.size() - 1) total_texture_h += lineGap;
        }

        if (max_texture_w > glm_max_texture_size) {
            GLM_LOG("[GLM] item text texture clamped %d -> %d\n", max_texture_w, glm_max_texture_size);
            max_texture_w = glm_max_texture_size;
        }
        if (total_texture_h > glm_max_texture_size) total_texture_h = glm_max_texture_size;
        if (max_texture_w <= 0 || total_texture_h <= 0) { 
            for(auto& lrd : finalLines) { for(auto& sd: lrd.segments) hb_buffer_destroy(sd.buf); }
            return; 
        }

        unsigned char* bmp = new (std::nothrow) unsigned char[(size_t)max_texture_w * total_texture_h * 4]();
        if (!bmp) {
            for (auto& fl : finalLines)
                for (auto& sd : fl.segments)
                    if (sd.buf) hb_buffer_destroy(sd.buf);
            return;
        }
        for (int i = 0; i < max_texture_w * total_texture_h; i++) { 
            bmp[i*4]=255; bmp[i*4+1]=255; bmp[i*4+2]=255; bmp[i*4+3]=0; 
        }

        int current_y_offset = 0;
        for (auto& lrd : finalLines) {
            float x = 0.0f;
            std::string align = (!item.customAlign.empty()) ? item.customAlign : w.itemTextAlign;
            if (align == "center") { x = (max_texture_w - lrd.tw) / 2.0f; } 
            else if (align == "right") { x = max_texture_w - lrd.tw; }

            for (auto& sd : lrd.segments) {
                FT_Set_Pixel_Sizes(sd.f, 0, lrd.fontSize);
                std::vector<bool> addedSpace(sd.text.length(), false);
                for (unsigned int i = 0; i < sd.count; ++i) {
                    FT_Load_Glyph(sd.f, sd.info[i].codepoint, FT_LOAD_RENDER);
                    FT_Bitmap* ft_bmp = &sd.f->glyph->bitmap;
                    int px_x = (int)x + (sd.pos[i].x_offset >> 6) + sd.f->glyph->bitmap_left;
                    int px_y = current_y_offset + lrd.baseline - (sd.pos[i].y_offset >> 6) - sd.f->glyph->bitmap_top;

                    for (unsigned int r = 0; r < ft_bmp->rows; ++r) {
                        for (unsigned int c = 0; c < ft_bmp->width; ++c) {
                            int final_x = px_x + c;
                            int final_y = px_y + r;
                            if (final_x >= 0 && final_x < max_texture_w && final_y >= 0 && final_y < total_texture_h) {
                                int idx = (final_y * max_texture_w + final_x) * 4;
                                bmp[idx] = (unsigned char)(255 * lrd.colorDim);
                                bmp[idx+1] = (unsigned char)(255 * lrd.colorDim);
                                bmp[idx+2] = (unsigned char)(255 * lrd.colorDim);
                                bmp[idx+3] = ft_bmp->buffer[r * ft_bmp->pitch + c];
                            }
                        }
                    }
                    x += sd.pos[i].x_advance >> 6;
                    if (lrd.extraSpace > 0.0f && sd.info[i].cluster < sd.text.length()) {
                        if (sd.text[sd.info[i].cluster] == ' ' && !addedSpace[sd.info[i].cluster]) {
                            x += lrd.extraSpace;
                            addedSpace[sd.info[i].cluster] = true;
                        }
                    }
                }
                hb_buffer_destroy(sd.buf);
            }
            current_y_offset += lrd.h + lineGap;
        }

        // ⚠️ ملكية النسيج للكاش: لا نحذف الـ ID القديم هنا (قد يخدم عنصراً آخر)
        item.textTexId = 0;
        glGenTextures(1, &item.textTexId);
        glBindTexture(GL_TEXTURE_2D, item.textTexId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); 
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1); 
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, max_texture_w, total_texture_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp); 
        delete[] bmp;
        
        // 🚀 2. حفظ أبعاد السطر الأول لتمريره للمحرك أثناء الرسم
        item.textW = max_texture_w; 
        item.textH = total_texture_h;
        item.textFirstLineW = finalLines[0].tw;
        item.textFirstLineH = finalLines[0].h + lineGap;
        if (stringLines.size() == 1) item.textFirstLineH = total_texture_h;
        
        globalTextCache.put(cacheKey, CachedText{item.textTexId, max_texture_w, total_texture_h,
                                                 (int)item.textFirstLineW, (int)item.textFirstLineH});
    }

    void create_widget_icon_texture(WidgetElement& w, WidgetItem& item) {
        if (item.iconCodepoint == 0 || w.iconFontPath.empty()) return;
        std::string fontCandidate = (!w.iconFontPath.empty() && w.iconFontPath[0] == '/') ? w.iconFontPath : currentScreen.pluginBasePath + w.iconFontPath;
        FT_Face face = get_ft_face(fontCandidate); if (!face) return;
        FT_Set_Pixel_Sizes(face, 0, w.iconFontSize);
        FT_Load_Char(face, item.iconCodepoint, FT_LOAD_RENDER);
        FT_Bitmap* ft_bmp = &face->glyph->bitmap;
        int tw = ft_bmp->width, h = ft_bmp->rows; if (tw <= 0 || h <= 0) return;

        unsigned char* bmp = new (std::nothrow) unsigned char[(size_t)tw * h * 4]();
        if (!bmp) return;
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < tw; c++) {
                int idx = (r * tw + c) * 4; bmp[idx]=255; bmp[idx+1]=255; bmp[idx+2]=255; bmp[idx+3]=ft_bmp->buffer[r * ft_bmp->pitch + c];
            }
        }
        if (item.iconTexId != 0) glDeleteTextures(1, &item.iconTexId);
        glGenTextures(1, &item.iconTexId);
        glBindTexture(GL_TEXTURE_2D, item.iconTexId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); 
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // 🚀
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // 🚀
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp); delete[] bmp;
        item.iconW = tw; item.iconH = h;
    }

    // 🚀 دالة جديدة لتوليد صورة توقيت الفيديو (Duration Badge)
    void create_badge_texture(WidgetElement& w, WidgetItem& item) {
        if (item.badgeText.empty()) return;
        std::string fontCandidate = w.fontPath.empty() ? "fonts/M-Bold.ttf" : w.fontPath;
        fontCandidate = (!fontCandidate.empty() && fontCandidate[0] == '/') ? fontCandidate : currentScreen.pluginBasePath + fontCandidate;
        int fontSize = 18; // حجم الخط للتوقيت
        std::string cacheKey = "badge_full_" + fontCandidate + "_" + std::to_string(fontSize) + "_" + item.badgeText;
        
        if (const CachedText* c = globalTextCache.get(cacheKey)) {
            item.badgeTexId = c->texId;
            item.badgeW     = (float)c->w;
            item.badgeH     = (float)c->h;
            return;
        }

        FT_Face face = get_ft_face(fontCandidate); if (!face) return;
        FT_Set_Pixel_Sizes(face, 0, fontSize);
        hb_font_t *hb_font = hb_font_cache[fontCandidate]; hb_ft_font_changed(hb_font);
        hb_buffer_t *hb_buffer = hb_buffer_create();
        hb_buffer_add_utf8(hb_buffer, item.badgeText.c_str(), -1, 0, -1);
        hb_buffer_set_direction(hb_buffer, HB_DIRECTION_LTR); hb_buffer_guess_segment_properties(hb_buffer);
        hb_shape(hb_font, hb_buffer, NULL, 0);

        unsigned int count; hb_glyph_info_t *info = hb_buffer_get_glyph_infos(hb_buffer, &count); hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(hb_buffer, &count);
        int tw = 0; for (unsigned int i = 0; i < count; ++i) tw += pos[i].x_advance >> 6;
        int max_asc = face->size->metrics.ascender >> 6; int max_desc = std::abs(face->size->metrics.descender >> 6);
        int th = max_asc + max_desc + 4; int baseline = max_asc + 2;
        if (tw <= 0 || th <= 0) { hb_buffer_destroy(hb_buffer); return; }

        // 🚀 Badge كامل مسبق التجهيز: خلفية سوداء + النص في Texture واحدة.
        // هذا يستبدل رسمتين + rounded shader برسم واحد فقط ويحافظ على ظهور مدة الفيديو في Grid أثناء الحركة.
        int bw = tw + 12;
        int bh = th + 6;
        unsigned char* bmp = new (std::nothrow) unsigned char[(size_t)bw * bh * 4]();
        if (!bmp) { hb_buffer_destroy(hb_buffer); return; }
        float radius = 4.0f;
        for (int yy = 0; yy < bh; yy++) {
            for (int xx = 0; xx < bw; xx++) {
                float dx = std::max(std::max(radius - xx, xx - (bw - 1 - radius)), 0.0f);
                float dy = std::max(std::max(radius - yy, yy - (bh - 1 - radius)), 0.0f);
                float dist = sqrtf(dx*dx + dy*dy);
                unsigned char a = (dist <= radius) ? 204 : 0;
                int idx = (yy * bw + xx) * 4;
                bmp[idx] = 0; bmp[idx+1] = 0; bmp[idx+2] = 0; bmp[idx+3] = a;
            }
        }

        float x = 0.0f;
        for (unsigned int i = 0; i < count; ++i) {
            FT_Load_Glyph(face, info[i].codepoint, FT_LOAD_RENDER); FT_Bitmap* ft_bmp = &face->glyph->bitmap;
            int px_x = 6 + (int)x + (pos[i].x_offset >> 6) + face->glyph->bitmap_left;
            int px_y = 3 + baseline - (pos[i].y_offset >> 6) - face->glyph->bitmap_top;
            for (unsigned int r = 0; r < ft_bmp->rows; ++r) {
                for (unsigned int c = 0; c < ft_bmp->width; ++c) {
                    int final_x = px_x + c; int final_y = px_y + r;
                    if (final_x >= 0 && final_x < bw && final_y >= 0 && final_y < bh) {
                        unsigned char ga = ft_bmp->buffer[r * ft_bmp->pitch + c];
                        int idx = (final_y * bw + final_x) * 4;
                        bmp[idx] = 255; bmp[idx+1] = 255; bmp[idx+2] = 255; bmp[idx+3] = ga;
                    }
                }
            }
            x += pos[i].x_advance >> 6;
        }
        hb_buffer_destroy(hb_buffer);
        glGenTextures(1, &item.badgeTexId); glBindTexture(GL_TEXTURE_2D, item.badgeTexId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp); delete[] bmp;
        item.badgeW = (float)bw; item.badgeH = (float)bh;
        globalTextCache.put(cacheKey, CachedText{item.badgeTexId, bw, bh, 0, 0});
    }

    // ✅ دالة لتحميل الصورة (PNG/JPG) وإنشاء Texture
    void load_image_texture(ImageElement& img) {
        if (img.imagePath.empty()) return;
        
        int width, height, nrChannels;
        // ✅ إجبار المكتبة على استخراج 4 قنوات (RGBA) دائماً لتفادي الـ Segmentation Fault
        unsigned char *data = stbi_load(img.imagePath.c_str(), &width, &height, &nrChannels, 4);
        
        if (!data) {
            return;
        }

        // ✅ حفظ نوع الصورة الأصلية
        img.hasAlpha = (nrChannels == 4 || img.imagePath.find(".png") != std::string::npos || img.imagePath.find(".PNG") != std::string::npos);

        // 🚀 السحر هنا: تنظيف البيكسلات الشفافة لمنع ظهور الحواف البيضاء (White Halo Fix)
        if (img.hasAlpha) {
            for (int i = 0; i < width * height * 4; i += 4) {
                if (data[i + 3] == 0) { // إذا كان البيكسل شفافاً تماماً (Alpha = 0)
                    data[i] = 0;     // تصفير اللون الأحمر
                    data[i + 1] = 0; // تصفير اللون الأخضر
                    data[i + 2] = 0; // تصفير اللون الأزرق
                }
            }
        }

        if (img.textureId != 0) glDeleteTextures(1, &img.textureId);
        glGenTextures(1, &img.textureId);
        glBindTexture(GL_TEXTURE_2D, img.textureId);

        // ✅ تحسين الأداء الخارق
        GLint filter = (width >= 1280 && height >= 720) ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

        stbi_image_free(data);
        img.loaded = true;
    }

    

    // ==================================================================================
    // دوال إدارة الذاكرة الذكية (Lazy Loading)
    // ==================================================================================
    // ❌ حُذفت get_free_pool_slot(): لم تكن مستدعاة من أي مكان، وكانت تحتوي على
    //    سباق TOCTOU (تفحص inUse ثم تحجزها بلا قفل). الحجز يتم الآن داخل
    //    load_widget_item_texture / load_widget_avatar_texture تحت queueMutex.

    void load_widget_item_texture(WidgetElement& w, WidgetItem& item, const std::string& basePath) {
        if (item.imagePath.empty() || item.imagePath == "none") return;
        
        std::lock_guard<std::mutex> lock(queueMutex);

        // 🚀 إذا كانت موجودة في الطابور مسبقاً، اسحبها وضعها في قمة المكدس لتُحمل فوراً!
        if (item.poolIndex != -1) {
            auto it = std::find(loadQueue.begin(), loadQueue.end(), item.poolIndex);
            if (it != loadQueue.end()) {
                loadQueue.erase(it);
                loadQueue.push_back(item.poolIndex);
                loaderCv.notify_one();
            }
            return;
        }

        int poolIdx = -1;
        for (int i = 0; i < MAX_TEXTURE_POOL; i++) {
            // 🚀 السحر الحقيقي (Memory Lock):
            // لا تعطِ هذه الخانة لبوستر جديد إلا إذا كانت غير مستخدمة، 
            // ولا يوجد Thread يعمل عليها، ولا توجد بيانات تنتظر الرفع للـ GPU!
            if (!texturePool[i].inUse && !texturePool[i].isLoading && !texturePool[i].dataReady) {
                texturePool[i].inUse = true;
                texturePool[i].domColor.store(-1);   // 🎨 خانة جديدة ⇒ لون غير محسوب بعد
                poolIdx = i;
                break;
            }
        }
        if (poolIdx == -1) {
            // مسبح النسيج ممتلئ: العنصر سيظهر فارغاً. نسجّلها لأنها تعطي نفس
            // عَرَض "العنصر المفقود" تماماً لكن لسبب مختلف (سعة المسبح لا النافذة).
            GLM_LOG("[GLM] texture pool exhausted (%d slots) - item not loaded: %s\n",
                    MAX_TEXTURE_POOL, item.imagePath.c_str());
            return;
        }

        std::string fullPath;
        if (item.imagePath.find("http://") == 0 || item.imagePath.find("https://") == 0) {
            fullPath = item.imagePath;
        } else {
            fullPath = (!item.imagePath.empty() && item.imagePath[0] == '/') ? item.imagePath : basePath + item.imagePath;
        }
        
        texturePool[poolIdx].pathToLoad = fullPath;
        texturePool[poolIdx].isLoading = true;
        texturePool[poolIdx].dataReady = false;
        texturePool[poolIdx].hasReflection = w.hasReflection; // ✅ تمرير أمر الانعكاس للمسبح
        texturePool[poolIdx].hasAlpha = false;
        // 🚀 إذا كان itemcornerDia موجوداً، نخبزه داخل pixels مرة واحدة في عامل التحميل.
        // نترك circle والـ reflection للشادر القديم لتجنب تغيير سلوك البلوجينات الخاصة.
        //texturePool[poolIdx].preRounded = (!w.hasReflection && w.itemType != "circle" && w.itemCornerRadius > 0.0f);
        // 🚀 تعطيل قص المعالج لإجبار كرت الشاشة على تنعيم الحواف بدقة عالية
        texturePool[poolIdx].preRounded = (!w.hasReflection && w.itemType != "circle" && w.itemCornerRadius > 0.0f);
        texturePool[poolIdx].fitCover   = w.itemFitCover;
        texturePool[poolIdx].cornerRadius = w.itemCornerRadius;
        texturePool[poolIdx].targetW = (item.customW > 0.0f) ? item.customW : w.itemW;
        texturePool[poolIdx].targetH = (item.customH > 0.0f) ? item.customH : w.itemH;
        
        item.poolIndex = poolIdx;
        item.loaded = false;

        loadQueue.push_back(poolIdx); // إضافتها لقمة المكدس (LIFO)
        loaderCv.notify_one();        // إيقاظ عامل واحد فوراً
    }

    void unload_widget_item_texture(WidgetItem& item) {
        if (item.poolIndex != -1) {
            // 🚀 إضافة القفل هنا لحماية المتغيرات أثناء التنقل السريع جداً
            std::lock_guard<std::mutex> lock(queueMutex);
            
            texturePool[item.poolIndex].inUse = false;
            // ⚠️ تحذير: لا تصفر isLoading هنا، اترك العامل (Worker Thread) يصفرها لكي لا يحدث تداخل!
            texturePool[item.poolIndex].dataReady = false;
            texturePool[item.poolIndex].hasAlpha = false;
            texturePool[item.poolIndex].preRounded = false;
            texturePool[item.poolIndex].cornerRadius = 0.0f;
            texturePool[item.poolIndex].targetW = 0.0f;
            texturePool[item.poolIndex].targetH = 0.0f;
            
            item.textureId = 0;
            item.poolIndex = -1;
            item.loaded = false;
        }
        // 🚀 تفريغ الأفاتار إذا تم التخلص من الفيديو
        if (item.avatarPoolIndex != -1) {
            std::lock_guard<std::mutex> lock(queueMutex);
            texturePool[item.avatarPoolIndex].inUse = false;
            texturePool[item.avatarPoolIndex].dataReady = false;
            item.avatarTexId = 0;
            item.avatarPoolIndex = -1;
            item.avatarLoaded = false;
        }
    }

    // 🚀 دالة جديدة لتحميل صورة القناة (Avatar) باستخدام المسبح العالمي
    void load_widget_avatar_texture(WidgetElement& w, WidgetItem& item, const std::string& basePath) {
        if (item.avatarPath.empty() || item.avatarPath == "none") return;
        std::lock_guard<std::mutex> lock(queueMutex);
        if (item.avatarPoolIndex != -1) {
            auto it = std::find(loadQueue.begin(), loadQueue.end(), item.avatarPoolIndex);
            if (it != loadQueue.end()) {
                loadQueue.erase(it);
                loadQueue.push_back(item.avatarPoolIndex);
                loaderCv.notify_one();
            }
            return;
        }
        int poolIdx = -1;
        for (int i = 0; i < MAX_TEXTURE_POOL; i++) {
            if (!texturePool[i].inUse && !texturePool[i].isLoading && !texturePool[i].dataReady) {
                texturePool[i].inUse = true; poolIdx = i; break;
            }
        }
        if (poolIdx == -1) return;
        std::string fullPath;
        if (item.avatarPath.find("http://") == 0 || item.avatarPath.find("https://") == 0) fullPath = item.avatarPath;
        else fullPath = (!item.avatarPath.empty() && item.avatarPath[0] == '/') ? item.avatarPath : basePath + item.avatarPath;
        texturePool[poolIdx].pathToLoad = fullPath;
        texturePool[poolIdx].isLoading = true;
        texturePool[poolIdx].dataReady = false;
        texturePool[poolIdx].hasReflection = false; // لا نحتاج انعكاس للوجو القناة
        item.avatarPoolIndex = poolIdx;
        item.avatarLoaded = false;
        loadQueue.push_back(poolIdx);
        loaderCv.notify_one();
    }

    // 🚀 تعريف أولي (Forward Declaration) لكي يتعرف عليها المترجم
    void create_widget_text_texture(WidgetElement& w, WidgetItem& item);

    void manage_widget_textures(WidgetElement& w) {
        // 🏷️ تحويل واحد بدل ثماني مقارنات نصية في كل استدعاء
        const OrientId wOrient = orient_id_of(w.orientation);
        // 🚀 الجدار الناري: إذا كانت القائمة فارغة (مثلما يحدث عند مسحها)، اخرج فوراً لمنع الكراش!
        if (w.items.empty()) return;

        float gap = w.itemGap;
        int currentIndex = 0;
        float step = (wOrient == OrientId::Vertical || wOrient == OrientId::Grid) ? w.itemH + gap : w.itemW + gap;
        if (step > 0) {
            int visualIndex = (int)(w.currentScrollOffset / step);
            currentIndex = (wOrient == OrientId::Grid) ? visualIndex * w.gridColumns : visualIndex;
        }
        if (currentIndex < 0) currentIndex = 0;

        if (!w.isAnimating) {
            currentIndex = w.selectedIndex;
        }

        int minIndex = std::min(w.selectedIndex, currentIndex);
        int maxIndex = std::max(w.selectedIndex, currentIndex);

        //int itemsOnScreen = 8;     
        //int offScreenBuffer = 6;
        
        int itemsOnScreen = (wOrient == OrientId::Grid) ? (w.gridColumns * 4) : 8;     
        int offScreenBuffer = (wOrient == OrientId::Grid) ? (w.gridColumns * 2) : 6;

        // ======================================================================
        // 🚀 إصلاح: عدد العناصر المرئية يُحسب من مقاس الويدجت الحقيقي لا من رقم ثابت
        // ======================================================================
        // كانت القيمة ثابتة (8 + 6) ⇒ نافذة توليد النصوص = التحديد ± 14 عنصر فقط.
        // أي قائمة عمودية تعرض أكثر من ~15 عنصراً كانت تفقد نصوص عناصرها السفلية
        // فتظهر فارغة، ولا "تمتلئ" إلا بالضغط على Down عنصراً عنصراً (النافذة تتقدم معه).
        // الآن نحسب كم عنصراً يتسع له الويدجت فعلياً ونوسّع النافذة تلقائياً.
        if (step > 1.0f) {
            float visibleExtent = (wOrient == OrientId::Horizontal) ? w.w : w.h;
            int visibleOnScreen = (int)ceil(visibleExtent / step) + 1;
            if (wOrient == OrientId::Grid) visibleOnScreen *= std::max(1, w.gridColumns);
            if (visibleOnScreen > itemsOnScreen) {
                itemsOnScreen   = visibleOnScreen;
                offScreenBuffer = std::max(offScreenBuffer, visibleOnScreen / 2);
            }
        }

        // 🚀 استثناء لقائمة الحروف لكي يتم شحنها بالكامل
        if (w.name == "az_list") { itemsOnScreen = std::max(itemsOnScreen, 30); }

        // ======================================================================
        // نافذة التحميل يجب أن تكون متماثلة حول التحديد
        // ======================================================================
        // كانت تمتد offScreenBuffer فقط إلى الخلف، مقابل itemsOnScreen + offScreenBuffer
        // إلى الأمام. هذا يصحّ ما دام التحديد في **بداية** المشهد المرئي، وهو الحال
        // أثناء التمرير العادي. لكن عند آخر عناصر القائمة لا تستطيع القائمة الانزلاق
        // أكثر، فيصبح التحديد في **نهاية** المشهد وتمتد العناصر المرئية إلى الخلف.
        //
        // مثال واقعي (صف بوسترات أفقي يعرض 8 من 40، التحديد على الأخير 39):
        //     المرئي فعلياً = 32..39     لكن  imgStart = 39 - 6 = 33
        //   ⇒ العنصر 32 مرئي على الشاشة لكنه خارج النافذة، فيُفرَّغ نسيجه ويظهر فارغاً.
        //   وبالرجوع خطوة واحدة يصبح 38 - 6 = 32 فيُحمَّل ويظهر من جديد.
        //   وهذا بالضبط السلوك المرصود: أول عنصر يختفي عند آخر القائمة ويعود بالرجوع.
        //
        // التوسيع للخلف بنفس مقدار الأمام يغطي الحالتين، ولا يمكنه أبداً أن يُنقص
        // أي تحميل قائم — النافذة الجديدة تحتوي القديمة بالكامل.
        const int spanBack    = itemsOnScreen + offScreenBuffer;
        const int spanForward = itemsOnScreen + offScreenBuffer;

        int imgStart = std::max(0, minIndex - spanBack);
        int imgEnd   = std::min((int)w.items.size() - 1, maxIndex + spanForward);

        int textStart = imgStart;
        int textEnd   = imgEnd;

        for (int i = 0; i < (int)w.items.size(); i++) {
            // --- إدارة الصور ---
            if (i >= imgStart && i <= imgEnd) {
                if (!w.items[i].loaded && w.items[i].imagePath != "none" && w.items[i].imagePath != "loading_state" && !w.items[i].imagePath.empty()) {
                    load_widget_item_texture(w, w.items[i], currentScreen.pluginBasePath);
                }
                // 🚀 إضافة الأفاتار للتحميل في الخلفية
                if (!w.items[i].avatarLoaded && w.items[i].avatarPath != "none" && !w.items[i].avatarPath.empty()) {
                    load_widget_avatar_texture(w, w.items[i], currentScreen.pluginBasePath);
                }
            } else {
                unload_widget_item_texture(w.items[i]);
            }

        }
        // --- إدارة النصوص (الوضع الطبيعي والمستقر لجميع الويدجت) ---
        int textsGeneratedThisFrame = 0;
        // 🚀 ميزانية أصغر: توليد النص يمرّ على FreeType ثم رفع نسيج للـ GPU،
        //    وأربعون نسيجاً في إطار واحد = إطار طويل جداً على معالج الرسيفر.
        //    الباقي يُستكمل في الإطار التالي بفضل علم textPending.
        int maxTextsAllowed = 10;   // نفس الميزانية للجميع؛ الباقي يكتمل في الإطار التالي

        // 🚀 الحماية القصوى: كبح توليد النصوص أثناء أي حركة (انزلاق عمودي أو شبكة) لمنع التشنج
        if (w.isAnimating) { 
            if (wOrient == OrientId::Grid) {
                // أثناء الحركة نولّد قليلاً جداً لكل إطار حتى لا تتقطع،
                // والبقية تكتمل تلقائياً بعد انتهاء الحركة (textPending).
                maxTextsAllowed = std::min(maxTextsAllowed, 3); 
            } else if (wOrient == OrientId::Vertical) {
                maxTextsAllowed = 6; // 🚀 السماح بنصين فقط في الفريم الواحد أثناء التنقل العمودي السريع
            }
        }

        // ✅ جديد: نفس الكبح أثناء انزلاق الشاشة/الطبقة كاملة (دخول أو خروج).
        // بدونه كان أي استدعاء من بايثون وسط الأنيميشن (مثل مؤقّت إجبار النصوص)
        // يولّد حتى 40 نسيجاً في إطار واحد ⇒ إطار طويل جداً ⇒ رمشة/قفزة في الحركة.
        // ما يُؤجَّل هنا يُستكمل في الإطارات التالية بفضل glm_request_render أدناه.
        if (currentScreen.slideActive) {
            maxTextsAllowed = std::min(maxTextsAllowed, (wOrient == OrientId::Grid) ? 4 : 6);
        }
        bool textGenDeferred = false;
        
        // // 🚀 الحماية القصوى: كبح توليد النصوص أثناء الحركة لتوجيه طاقة المعالج 100% للأنيميشن
        // if (w.isAnimating) { 
        //     if (wOrient == OrientId::Grid) {
        //         maxTextsAllowed = 0; // 🚀 منع توليد أي نصوص جديدة كلياً أثناء انزلاق الشبكة لمنع التقطيع
        //     } else if (wOrient == OrientId::Vertical) {
        //         maxTextsAllowed = 2; 
        //     } else {
        //         maxTextsAllowed = 2; // للكاروسال الأفقي
        //     }
        // }
        
        // 🚀 استثناء لقائمة الحروف لكي لا يقوم المحرك بإخفائها
        if (w.name == "az_list") { maxTextsAllowed = 30; }
        for (int i = 0; i < (int)w.items.size(); i++) {
            // --- إدارة النصوص والأيقونات بذكاء (الـ Lazy Loading) ---
            if (i >= textStart && i <= textEnd) {
                // 🚀 الحل السحري: كبح التوليد لحماية الخيط الرئيسي
                if (w.items[i].textTexId == 0 && !w.items[i].text.empty()) {
                    //if (textsGeneratedThisFrame < 2) { // لن نسمح بأكثر من نصين في الفريم الواحد
                    // 🚀 السحر هنا: فحص الكاش السريع أولاً قبل استهلاك حصة التوليد (Lazy Loading حقيقي)
                    std::string fontCandidate = w.fontPath.empty() ? "fonts/M-Bold.ttf" : w.fontPath;
                    fontCandidate = (!fontCandidate.empty() && fontCandidate[0] == '/')
                                    ? fontCandidate : currentScreen.pluginBasePath + fontCandidate;
                    std::string cacheKey = fontCandidate + "_" + std::to_string(w.fontSize) + "_" + w.items[i].text;

                    if (const CachedText* c = globalTextCache.get(cacheKey)) {
                        w.items[i].textTexId = c->texId;
                        w.items[i].textW     = (float)c->w;
                        w.items[i].textH     = (float)c->h;
                        // ✅ إصلاح: استرجاع أبعاد السطر الأول أيضاً،
                        // وإلا بقي textFirstLineW = 0 فلا يعمل scrolltext إطلاقاً.
                        w.items[i].textFirstLineW = (float)c->firstLineW;
                        w.items[i].textFirstLineH = (float)c->firstLineH;
                    } else if (textsGeneratedThisFrame < maxTextsAllowed) {
                        create_widget_text_texture(w, w.items[i]);
                        textsGeneratedThisFrame++;
                    } else { textGenDeferred = true; }
                }
                
                if (w.items[i].iconTexId == 0 && w.items[i].iconCodepoint != 0) {
                    if (textsGeneratedThisFrame < maxTextsAllowed) {
                        create_widget_icon_texture(w, w.items[i]);
                        textsGeneratedThisFrame++;
                    } else { textGenDeferred = true; }
                }
                // 🚀 توليد توقيت الفيديو
                if (w.items[i].badgeTexId == 0 && !w.items[i].badgeText.empty()) {
                    if (textsGeneratedThisFrame < maxTextsAllowed) {
                        create_badge_texture(w, w.items[i]);
                        textsGeneratedThisFrame++;
                    } else { textGenDeferred = true; }
                }
            }
        }

        // ✅ ما تأجّل توليده يُستكمل في الإطارات التالية.
        // ⚠️ كان الاكتفاء بـ glm_request_render() لا يكفي: إعادة الرسم وحدها لا
        //    تولّد النصوص المتبقية لأن هذه الدالة لا تُستدعى إلا عند تغيّر التحديد،
        //    فكانت النصوص لا تظهر حتى يضغط المستخدم زراً آخر. الآن نرفع علماً
        //    يقرؤه مسار الرسم في كل إطار حتى تكتمل كل النصوص المرئية.
        w.textPending = textGenDeferred;
        if (textGenDeferred) glm_request_render();

        // 🚀 حلقة Center-Outward المحمية والمؤمنة 100% ضد الكراش
        int centerIdx = w.selectedIndex;
        if (centerIdx >= (int)w.items.size()) centerIdx = (int)w.items.size() - 1;
        if (centerIdx < 0) centerIdx = 0;

        int maxDist = std::max(centerIdx - imgStart, imgEnd - centerIdx);
        
        for (int dist = 0; dist <= maxDist; dist++) {
            int r = centerIdx + dist;
            if (r >= imgStart && r <= imgEnd && r < (int)w.items.size() && r >= 0) {
                if (!w.items[r].loaded && w.items[r].imagePath != "none" && !w.items[r].imagePath.empty()) {
                    load_widget_item_texture(w, w.items[r], currentScreen.pluginBasePath);
                }
                if (!w.items[r].avatarLoaded && w.items[r].avatarPath != "none" && !w.items[r].avatarPath.empty()) {
                    load_widget_avatar_texture(w, w.items[r], currentScreen.pluginBasePath);
                }
            }
                
            int l = centerIdx - dist;
            if (l >= imgStart && l <= imgEnd && l < (int)w.items.size() && l >= 0) {
                // ✅ تم التصحيح هنا: تغيير .empty() إلى .imagePath.empty()
                if (!w.items[l].loaded && w.items[l].imagePath != "none" && !w.items[l].imagePath.empty()) {
                    load_widget_item_texture(w, w.items[l], currentScreen.pluginBasePath);
                }
                if (!w.items[l].avatarLoaded && w.items[l].avatarPath != "none" && !w.items[l].avatarPath.empty()) {
                    load_widget_avatar_texture(w, w.items[l], currentScreen.pluginBasePath);
                }
            }
        }
    }

    
    
    

    void internal_update_widget_items(const char* widgetName, const char* pathsString) {
        std::string nameStr = widgetName ? widgetName : "";
        std::string pathsStr = pathsString ? pathsString : "";

        // 🚀 1. السحر هنا: معالجة النصوص الثقيلة خارج القفل (بدون إيقاف الشاشة أو تجميد الكاروسال)
        std::vector<WidgetItem> newParsedItems;
        std::string firstNewImagePath = "";
        
        std::stringstream ss(pathsStr);
        std::string token;
        bool isFirst = true;

        while (std::getline(ss, token, '\n')) {
            if (token.empty()) continue;
            
            WidgetItem item;
            std::stringstream ss2(token);
            std::string part;
            int pIndex = 0;
            while (std::getline(ss2, part, ';')) {
                if (pIndex == 0) {
                    item.imagePath = part;
                    if (isFirst) { firstNewImagePath = part; isFirst = false; }
                }
                else if (pIndex == 1) item.text = part;
                else if (pIndex == 2) item.customAlign = part;
                else if (pIndex == 3) { try { item.customW = std::stof(part); } catch(...) {} }
                else if (pIndex == 4) { try { item.customH = std::stof(part); } catch(...) {} }
                else if (pIndex == 5) { try { item.customX = std::stof(part); } catch(...) {} }
                else if (pIndex == 6) { try { item.customY = std::stof(part); } catch(...) {} }
                else if (pIndex == 7) { try { item.iconCodepoint = std::stoul(part, nullptr, 16); } catch(...) {} }
                else if (pIndex == 8) { item.badgeText = part; }
                else if (pIndex == 9) { item.avatarPath = part; } // 🚀 قراءة الأفاتار
                else if (pIndex == 10) { try { item.progress = std::stof(part); } catch(...) {} } // 📊 نسبة شريط التقدّم 0..100 (سالب = مخفي)
                pIndex++;
            }
            newParsedItems.push_back(item);
        }

        // 🚀 2. الآن نقفل الشاشة بأسرع ما يمكن لتطبيق البيانات الجاهزة فقط!
        std::lock_guard<std::mutex> lock(screenMutex);
        // 👇 أضف هذين السطرين لإجبار المحرك على رسم الأبعاد الجديدة فوراً
        invalidate_layer_scene_cache();
        invalidate_static_scene_cache();
        
        for (auto& w : currentScreen.widgets) {
            if (w.name == nameStr) {
                std::vector<WidgetItem> oldItems = w.items; 
                
                bool isAppend = false;
                if (!oldItems.empty() && !firstNewImagePath.empty() && firstNewImagePath != "none" && firstNewImagePath != "loading_state") {
                    if (oldItems[0].imagePath == firstNewImagePath) {
                        isAppend = true;
                    }
                }

                w.items.clear();

                if (w.name != "sidebar_focus" && !isAppend) {
                    w.currentScrollOffset = 0.0f;
                    w.startScrollOffset = 0.0f;
                    w.selectedIndex = 0;
                    w.prevIndex = 0;
                    
                    // تم وضعها هنا لمنع قتل الأنيميشن عند دمج الصفحات
                    w.isAnimating = false;
                    w.textScrollActive = true;
                    w.textScrollStartTime = -1.0f;
                }

                int itemIndex = 0;
                for (auto& item : newParsedItems) {
                    bool foundImage = false;
                    bool foundText = false;
                    bool foundIcon = false;
                    bool foundAvatar = false;

                    if (itemIndex < (int)oldItems.size()) {
                        if (oldItems[itemIndex].imagePath == item.imagePath) {
                            item.textureId = oldItems[itemIndex].textureId;
                            item.poolIndex = oldItems[itemIndex].poolIndex;
                            item.loaded = oldItems[itemIndex].loaded;
                            item.domR = oldItems[itemIndex].domR;   // 🎨
                            item.domG = oldItems[itemIndex].domG;
                            item.domB = oldItems[itemIndex].domB;
                            item.domValid = oldItems[itemIndex].domValid;
                            oldItems[itemIndex].poolIndex = -1; 
                            foundImage = true;
                        }
                        if (oldItems[itemIndex].text == item.text) {
                            item.textTexId = oldItems[itemIndex].textTexId;
                            item.textW = oldItems[itemIndex].textW;
                            item.textH = oldItems[itemIndex].textH;
                            item.textFirstLineW = oldItems[itemIndex].textFirstLineW;
                            item.textFirstLineH = oldItems[itemIndex].textFirstLineH;
                            foundText = true;
                        }
                        if (oldItems[itemIndex].iconCodepoint == item.iconCodepoint) {
                            item.iconTexId = oldItems[itemIndex].iconTexId;
                            item.iconW = oldItems[itemIndex].iconW;
                            item.iconH = oldItems[itemIndex].iconH;
                            foundIcon = true;
                        }
                        if (oldItems[itemIndex].badgeText == item.badgeText) {
                            item.badgeTexId = oldItems[itemIndex].badgeTexId;
                            item.badgeW = oldItems[itemIndex].badgeW;
                            item.badgeH = oldItems[itemIndex].badgeH;
                        } else { item.badgeTexId = 0; }

                        // 🚀 فحص الأفاتار
                        if (oldItems[itemIndex].avatarPath == item.avatarPath && !item.avatarPath.empty()) {
                            item.avatarTexId = oldItems[itemIndex].avatarTexId;
                            item.avatarPoolIndex = oldItems[itemIndex].avatarPoolIndex;
                            item.avatarLoaded = oldItems[itemIndex].avatarLoaded;
                            oldItems[itemIndex].avatarPoolIndex = -1; // حماية
                            foundAvatar = true; 
                        }
                    }

                    if (!foundImage) { item.loaded = false; item.textureId = 0; item.poolIndex = -1; }
                    if (!foundText) { item.textTexId = 0; }
                    if (!foundIcon) { item.iconTexId = 0; }

                    if (!foundAvatar) { item.avatarLoaded = false; item.avatarTexId = 0; item.avatarPoolIndex = -1; }
                    
                    
                    w.items.push_back(item);
                    itemIndex++;
                }
                
                for (auto& oldItem : oldItems) {
                    unload_widget_item_texture(oldItem);
                }

                if (w.selectedIndex >= (int)w.items.size()) {
                    w.selectedIndex = std::max(0, (int)w.items.size() - 1);
                    w.prevIndex = w.selectedIndex;
                }
                
                manage_widget_textures(w);
                break;
            }
        }
    }

    

    // 🚀 تغيير اسم الدالة لتصبح مخصصة للمحرك الداخلي فقط
    void internal_set_background_image(const char* imgPath) {
        if (!imgPath) return;
        std::string path(imgPath);
        std::lock_guard<std::mutex> lock(screenMutex);

        if (path.empty() || path == currentScreen.currentBg.imagePath) return;

        // 🚀 إصلاح: قفل الحماية يمنع تلف الذاكرة بينما يقرأ العامل المسار
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            bgTaskPath = path;
            // 🎭 نلتقط قناع **الشاشة الطالبة** الآن. قراءته لاحقاً داخل العامل كانت
            //    تعطي قناع أي شاشة تصادف أن تكون فعّالة وقتها (أو لا شيء إطلاقاً).
            bgTaskMaskPath = currentScreen.maskPath;
            
            // 🚀 حل مشكلة الباكدروب السابق:
            // نتخلص من أي بيانات باكدروب تمت معالجتها للفيلم السابق ولم يتم رفعها بعد
            unsigned char* staleData = (unsigned char*)bgTaskData.exchange(nullptr);
            if (staleData) {
                stbi_image_free(staleData);
            }
        }
        bgTaskPending = true;   // إشارة للعامل للبدء
        loaderCv.notify_one();
    }

    // ==================================================================================
    // 5. قراءة XML (XML Parser) - النظيف
    // ==================================================================================
    // 🪟 النواة: تبني الشاشة داخل currentScreen. لا تأخذ القفل ولا تعرف شيئاً عن المكدّس،
    //    لذلك يستعملها كل من التحميل العادي (load) والتراكب (push).
    void internal_load_interface_xml_core(const char* xmlPath, const char* screenName, const char* pluginPath) {
        if (!xmlPath || !screenName) return;
        invalidate_static_scene_cache();
        invalidate_layer_scene_cache();
        {
            std::lock_guard<std::mutex> qlock(queueMutex);
            backdropMaskPath = "";
        }

        // 🚀 تصفير متغيرات الإخفاء فوراً لمنع انتقال التأثير إلى الشاشات الأخرى
        backdrop_hidden = false;
        ui_hidden = false;
        last_backdrop_hidden = false;
        trailer_fade_start = -1.0f;
        trailer_post_render_until = -1.0f;
        trailer_cover_visible = false;
        trailer_cover_alpha = 0.0f;
        trailer_cover_fade_start = -1.0f;

        // ✅ مسح كاش النصوص القديمة من GPU لمنع استخدام Texture IDs محذوفة.
        // clear() تحذف النسج وتصفّر أي عنصر يشير إليها (الكاش هو المالك).
        // 🪟 عند وجود طبقات متراكبة لا نُخلي الكاش: الطبقات السفلى ما زالت تملك
        //    نصوصاً حيّة ستحتاجها عند الـ pop، والإخلاء هنا سيُبطل معرّفاتها.
        //    glm_forget_text_textures يمسح المكدّس أيضاً، فلا خطر من مؤشر معلّق.
        if (screenStack.empty()) globalTextCache.clear();

        currentScreen.screenFadeState = 0;
        currentScreen.screenFadeStartTime = -1.0f;
        currentScreen.screenFadeDuration = 0.4f;

        // ✅ تصفير الفيد الخاص بالمعلومات عند تحميل أي شاشة جديدة لمنع تداخله
        currentScreen.isMaskFading = false;
        currentScreen.maskFadeStartTime = -1.0f;
        currentScreen.maskFadeDuration = 0.4f;

        // ✅ 1. تصفير إعدادات الباكدروب تماماً عند كل عملية تحميل جديدة
        currentScreen.backdropPath = "";
        currentScreen.maskPath     = "";
        currentScreen.bgBakedMask  = "";
        currentScreen.backdropX = 0.0f;
        currentScreen.backdropY = 0.0f;
        currentScreen.backdropW = 1920.0f;
        currentScreen.backdropH = 1080.0f;
        currentScreen.backdropZ = 0;
        
        // ✅ 2. تصفير الصور الحالية لمنع بقاء آخر خلفية ظاهرة
        currentScreen.currentBg.imagePath = "";
        currentScreen.currentBg.loaded = false;
        if (currentScreen.currentBg.textureId != 0) {
            glDeleteTextures(1, &currentScreen.currentBg.textureId);
            currentScreen.currentBg.textureId = 0;
        }
                
        
        // ✅ مسح النصوص والأيقونات القديمة من ذاكرة كرت الشاشة (GPU) لمنع تراكمها
        for (auto& lbl : currentScreen.labels) {
            // if (lbl.textureId != 0) {
            //     glDeleteTextures(1, &lbl.textureId);
            // }
            if (lbl.iconTextureId != 0) {
                glDeleteTextures(1, &lbl.iconTextureId);
            }
        }
        currentScreen.labels.clear();

        // ✅ مسح الصور القديمة وحذفها من ذاكرة كرت الشاشة (GPU) لمنع تراكمها
        for (auto& img : currentScreen.images) { if (img.textureId != 0) { glDeleteTextures(1, &img.textureId); } }
        // 🚀 الإصلاح السحري: إفراغ المسبح بأمان تام دون تدمير حاويات كرت الشاشة (OpenGL)
        for (auto& w : currentScreen.widgets) { 
            for (auto& item : w.items) { 
                unload_widget_item_texture(item); // ✅ يحرر الخانة في المسبح لكي تستقبل صوراً جديدة
                
                // ⚠️ تحذير: لا تحذف item.textureId أبداً هنا!
                item.textTexId  = 0;   // ⚠️ ملكية globalTextCache
                item.badgeTexId = 0;
                if (item.iconTexId != 0) glDeleteTextures(1, &item.iconTexId);
                if (item.avatarTexId != 0) glDeleteTextures(1, &item.avatarTexId);
            } 
            if (w.selectionTexId != 0) glDeleteTextures(1, &w.selectionTexId); 
        } 
        currentScreen.widgets.clear();

        currentScreen.images.clear();
        //currentScreen.slideActive = true; currentScreen.slideStartTime = -1.0f;
        currentScreen.pluginBasePath = pluginPath ? pluginPath : "";
        //currentScreen.bgR = 0; currentScreen.bgG = 0; currentScreen.bgB = 0;
        // 🚀 التعديل: التأكد من تصفير الشفافية عند الانتقال من شاشة لأخرى
        currentScreen.bgR = 0; currentScreen.bgG = 0; currentScreen.bgB = 0; currentScreen.bgA = 0.0f;

        // 🚀 الإصلاح الجذري: تصفير الأنيميشن الشامل لمنع وراثة الحركات السابقة واختفاء الصور (Ghost Animation)
        currentScreen.animType = "none";
        currentScreen.slideActive = false;
        currentScreen.slideStartTime = -1.0f;

        FILE* file = fopen(xmlPath, "r"); if (!file) return;
        char buffer[2048]; std::string content = ""; while (fgets(buffer, sizeof(buffer), file)) content += buffer; fclose(file);

        std::string screenBlock = "";
        // ✅ تغيير الوسم إلى Layout لفصله تماماً عن محرك Enigma2 الافتراضي
        size_t pos = content.find(std::string("<Layout name=\"") + screenName + "\"");
        if (pos != std::string::npos) { 
            size_t end = content.find("</Layout>", pos); 
            if (end != std::string::npos) screenBlock = content.substr(pos, end - pos + 9); 
        }
        if (screenBlock.empty()) return;

        // ✅ تنظيف التعليقات من الـ XML (لتجاهل أي شيء بين <!-- و -->)
        size_t commentStart = screenBlock.find("<!--");
        while (commentStart != std::string::npos) {
            size_t commentEnd = screenBlock.find("-->", commentStart);
            if (commentEnd != std::string::npos) {
                screenBlock.erase(commentStart, (commentEnd + 3) - commentStart);
            } else {
                screenBlock.erase(commentStart); // إذا كان التعليق مفتوح ولم يُغلق
                break;
            }
            commentStart = screenBlock.find("<!--");
        }

        // ✅ قراءة وسام Backdrop كاملاً للتحكم في مكان وحجم ومسار الـ Fanart
        size_t bPos = screenBlock.find("<Backdrop ");
        if (bPos != std::string::npos) {
            size_t bEnd = screenBlock.find("/>", bPos);
            if (bEnd != std::string::npos) {
                std::string bTag = screenBlock.substr(bPos, bEnd - bPos);
                size_t p;
                p = bTag.find("zPosition=\""); if (p != std::string::npos) { try { currentScreen.backdropZ = std::stoi(bTag.substr(p + GLM_ATTR_LEN("zPosition"))); } catch(...) {} }
                p = bTag.find("position=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("position"), e=bTag.find("\"",s); if(e!=std::string::npos){ std::string pos=bTag.substr(s,e-s); size_t c=pos.find(","); if(c!=std::string::npos){ try{currentScreen.backdropX=std::stof(pos.substr(0,c)); currentScreen.backdropY=std::stof(pos.substr(c+1));}catch(...){} } } }
                p = bTag.find("size=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("size"), e=bTag.find("\"",s); if(e!=std::string::npos){ std::string sz=bTag.substr(s,e-s); size_t c=sz.find(","); if(c!=std::string::npos){ try{currentScreen.backdropW=std::stof(sz.substr(0,c)); currentScreen.backdropH=std::stof(sz.substr(c+1));}catch(...){} } } }
                // ⏱️ مدة تلاشي تبديل الباكدروب بالثواني (0 = تبديل فوري بلا تلاشٍ).
                //    أثناء التلاشي يُرسم باكدروبان بملء الشاشة ويُعطَّل Layer FBO،
                //    فكل إطار يعيد رسم المشهد كاملاً — لذلك المدة تكلفة مباشرة.
                p = bTag.find("fadeSpeed=\"");
                if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("fadeSpeed"), e=bTag.find("\"",s); if(e!=std::string::npos) try{ currentScreen.bgFadeDuration = std::stof(bTag.substr(s,e-s)); }catch(...){} }

                // ✅ قراءة المسار (Path) المخصص للباكدروبات
                p = bTag.find("Path=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("Path"), e=bTag.find("\"",s); if(e!=std::string::npos){ currentScreen.backdropPath = bTag.substr(s,e-s); } }
                GLM_LOG("[GLM-ENGINE DEBUG] Successfully read Backdrop Path from XML: %s\n", currentScreen.backdropPath.c_str());
            }
        }

        // 🔊 قراءة وسم <VolumeBar /> لتخصيص شريط الصوت لكل شاشة على حدة
        {
            std::lock_guard<std::mutex> vlock(volumeStyleMutex);
            size_t vbPos = screenBlock.find("<VolumeBar");
            if (vbPos != std::string::npos) {
                size_t vbEnd = screenBlock.find("/>", vbPos);
                if (vbEnd != std::string::npos) {
                    std::string vTag = screenBlock.substr(vbPos, vbEnd - vbPos);
                    VolumeBarStyle st; // نبدأ من الافتراضي ثم نطبق ما في XML
                    size_t p;

                    auto attr = [&](const std::string& key, std::string& out) -> bool {
                        size_t kp = vTag.find(key + "=\"");
                        if (kp == std::string::npos) return false;
                        size_t s = kp + key.size() + 2, e = vTag.find("\"", s);
                        if (e == std::string::npos) return false;
                        out = vTag.substr(s, e - s);
                        return true;
                    };
                    auto attrColor = [&](const std::string& key, float& r, float& g, float& b, float& a) {
                        std::string v; if (attr(key, v)) glm_parse_hex_color(v, r, g, b, a);
                    };
                    auto attrFloat = [&](const std::string& key, float& out) {
                        std::string v; if (attr(key, v)) { try { out = std::stof(v); } catch(...) {} }
                    };
                    auto attrInt = [&](const std::string& key, int& out) {
                        std::string v; if (attr(key, v)) { try { out = std::stoi(v); } catch(...) {} }
                    };
                    auto attrFont = [&](const std::string& key, std::string& path, int& size) {
                        std::string v;
                        if (!attr(key, v)) return;
                        size_t c = v.find(";");
                        if (c != std::string::npos) {
                            path = v.substr(0, c);
                            try { size = std::stoi(v.substr(c + 1)); } catch(...) {}
                        } else path = v;
                    };
                    auto attrHex32 = [&](const std::string& key, uint32_t& out) {
                        std::string v;
                        if (!attr(key, v)) return;
                        if (v.size() > 2 && (v[0] == '0') && (v[1] == 'x' || v[1] == 'X')) v = v.substr(2);
                        try { out = (uint32_t)std::stoul(v, nullptr, 16); } catch(...) {}
                    };

                    std::string posStr;
                    if (attr("position", posStr)) {
                        size_t c = posStr.find(",");
                        if (c != std::string::npos) {
                            std::string xs = posStr.substr(0, c);
                            std::string ys = posStr.substr(c + 1);
                            if (xs == "center") { st.centerX = true; }
                            else { try { st.x = std::stof(xs); st.centerX = false; } catch(...) {} }
                            try { st.y = std::stof(ys); } catch(...) {}
                        }
                    }
                    std::string szStr;
                    if (attr("size", szStr)) {
                        size_t c = szStr.find(",");
                        if (c != std::string::npos) {
                            try { st.w = std::stof(szStr.substr(0, c)); st.h = std::stof(szStr.substr(c + 1)); } catch(...) {}
                        }
                    }

                    p = vTag.find("cornerDia=\"");
                    if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("cornerDia"), e=vTag.find("\"",s); if(e!=std::string::npos){ try{ st.radius = std::stof(vTag.substr(s,e-s))/2.0f; }catch(...){} } }
                    p = vTag.find("barCornerDia=\"");
                    if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("barCornerDia"), e=vTag.find("\"",s); if(e!=std::string::npos){ try{ st.barRadius = std::stof(vTag.substr(s,e-s))/2.0f; }catch(...){} } }

                    attrFloat("barHeight", st.barHeight);
                    attrColor("color",       st.panelR, st.panelG, st.panelB, st.panelA);
                    attrColor("trackColor",  st.trackR, st.trackG, st.trackB, st.trackA);
                    attrColor("fillColor",   st.fillR,  st.fillG,  st.fillB,  st.fillA);
                    attrColor("textColor",   st.textR,  st.textG,  st.textB,  st.textA);
                    attrColor("mutedColor",  st.muteR,  st.muteG,  st.muteB,  st.muteA);
                    { float ia = 1.0f; attrColor("iconColor", st.iconR, st.iconG, st.iconB, ia); }
                    attrFont("font", st.fontPath, st.fontSize);
                    attrFont("iconFont", st.iconFontPath, st.iconFontSize);
                    attrHex32("icon", st.iconCp);
                    attrHex32("mutedIcon", st.iconMutedCp);
                    attrInt("showPercent", st.showPercent);
                    { float tmo = volume_timeout.load(); attrFloat("timeout", tmo); volume_timeout.store(tmo); }

                    volumeStyle = st;
                    volume_style_from_python.store(false); // XML له الأولوية عند تحميل الشاشة
                }
            } else if (!volume_style_from_python.load()) {
                volumeStyle = VolumeBarStyle(); // شاشة بدون وسم = الشكل الافتراضي
            }
        }

        // ✅ قراءة التدرجات اللونية من XML
        currentScreen.gradients.clear();
        size_t grPos = screenBlock.find("<Gradient ");
        while (grPos != std::string::npos) {
            size_t grEnd = screenBlock.find("/>", grPos); if (grEnd == std::string::npos) break;
            std::string grTag = screenBlock.substr(grPos, grEnd - grPos); GradientElement gr; size_t p;
            
            p = grTag.find("position=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("position"), e=grTag.find("\"",s); if(e!=std::string::npos){ std::string pos=grTag.substr(s,e-s); size_t c=pos.find(","); if(c!=std::string::npos){ gr.x=std::stof(pos.substr(0,c)); gr.y=std::stof(pos.substr(c+1)); } } }
            p = grTag.find("size=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("size"), e=grTag.find("\"",s); if(e!=std::string::npos){ std::string sz=grTag.substr(s,e-s); size_t c=sz.find(","); if(c!=std::string::npos){ gr.w=std::stof(sz.substr(0,c)); gr.h=std::stof(sz.substr(c+1)); } } }
            
            auto parseColor = [&](std::string key, float& r, float& g, float& b, float& a) {
                size_t cp = grTag.find(key);
                if (cp != std::string::npos) {
                    size_t s=cp+key.length()+2, e=grTag.find("\"", s);
                    if (e != std::string::npos) {
                        std::string hex = grTag.substr(s, e-s);
                        if (hex.length() == 9 && hex[0] == '#') {
                            r = std::stoi(hex.substr(1,2),nullptr,16)/255.0f; g = std::stoi(hex.substr(3,2),nullptr,16)/255.0f;
                            b = std::stoi(hex.substr(5,2),nullptr,16)/255.0f; a = std::stoi(hex.substr(7,2),nullptr,16)/255.0f;
                        }
                    }
                }
            };
            parseColor("startColor", gr.r1, gr.g1, gr.b1, gr.a1);
            parseColor("endColor", gr.r2, gr.g2, gr.b2, gr.a2);
            currentScreen.gradients.push_back(gr);
            grPos = screenBlock.find("<Gradient ", grEnd + 2);
        }

        

        // 1. استخراج لون الخلفية (يدعم الشفافية)
        size_t bgPos = screenBlock.find("backgroundColor=\"");
        if (bgPos != std::string::npos) { 
            size_t s = bgPos + 17, e = screenBlock.find("\"", s); 
            if (e != std::string::npos) { 
                std::string hex = screenBlock.substr(s, e - s); 
                try {
                    if (hex.length() == 7 && hex[0] == '#') { // صيغة #RRGGBB
                        currentScreen.bgR = std::stoi(hex.substr(1,2),nullptr,16)/255.0f; 
                        currentScreen.bgG = std::stoi(hex.substr(3,2),nullptr,16)/255.0f; 
                        currentScreen.bgB = std::stoi(hex.substr(5,2),nullptr,16)/255.0f;
                        currentScreen.bgA = 1.0f; // معتم افتراضياً
                    } 
                    else if (hex.length() == 9 && hex[0] == '#') { // ✅ صيغة #RRGGBBAA
                        currentScreen.bgR = std::stoi(hex.substr(1,2),nullptr,16)/255.0f; 
                        currentScreen.bgG = std::stoi(hex.substr(3,2),nullptr,16)/255.0f; 
                        currentScreen.bgB = std::stoi(hex.substr(5,2),nullptr,16)/255.0f;
                        currentScreen.bgA = std::stoi(hex.substr(7,2),nullptr,16)/255.0f; // ✅ قراءة الشفافية
                    }
                } catch(...) {} 
            } 
        }

        // ✅ استخراج نوع الأنيميشن (Animation)
        size_t animPos = screenBlock.find("Animation=\"");
        if (animPos != std::string::npos) {
            size_t s = animPos + 11, e = screenBlock.find("\"", s);
            if (e != std::string::npos) {
                currentScreen.animType = screenBlock.substr(s, e - s);
            }
        }
        
        // ✅ تفعيل الأنيميشن فقط إذا كانت من النوع المدعوم
        if (currentScreen.animType == "slide_right" || currentScreen.animType == "slide_left" || 
            currentScreen.animType == "slide_right_fade" || currentScreen.animType == "slide_left_fade" ||
            currentScreen.animType == "slide_up_fade" || currentScreen.animType == "slide_down_fade" ||
            currentScreen.animType == "slide_up" || currentScreen.animType == "slide_down" ||
            currentScreen.animType == "zoom_fade") { // 🚀 دعم أنيميشن التكبير والتلاشي الشامل
            currentScreen.slideActive = true;
            currentScreen.slideStartTime = -1.0f;
        } else {
            currentScreen.slideActive = false; // أي شيء آخر أو "none" يعطل الأنيميشن
        }

        // 🎚️ استخراج منحنى التنعيم (Easing) — اختياري، الافتراضي quint كما كان.
        // نبحث داخل وسم <Layout ...> وحده حتى لا نلتقط خاصية تخص عنصراً ابناً.
        {
            size_t headEnd = screenBlock.find('>');
            std::string headTag = (headEnd == std::string::npos) ? screenBlock : screenBlock.substr(0, headEnd);
            size_t easePos = headTag.find("Easing=\"");
            if (easePos != std::string::npos) {
                size_t s2 = easePos + 8, e2 = headTag.find("\"", s2);
                if (e2 != std::string::npos) {
                    std::string raw = headTag.substr(s2, e2 - s2);
                    std::string low = "";
                    for (char c : raw) low += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
                    currentScreen.easeId = ease_id_of(low);
                }
            }
        }

        // ✅ استخراج سرعة الأنيميشن (Speed)
        size_t speedPos = screenBlock.find(" Speed=\"");
        if (speedPos != std::string::npos) {
            size_t s = speedPos + 8, e = screenBlock.find("\"", s);
            if (e != std::string::npos) {
                try {
                    float speedVal = std::stof(screenBlock.substr(s, e - s));
                    if (speedVal > 0.0f) {
                        currentScreen.animDuration = 0.8f / speedVal;
                    }
                } catch(...) {}
            }
        }

        // ✅ استخراج المسافة الشاملة (distance) من وسم Layout
        currentScreen.animDistance = 80.0f; // القيمة الافتراضية
        size_t distPos = screenBlock.find(" distance=\"");
        if (distPos == std::string::npos) distPos = screenBlock.find("distance=\"");
        if (distPos != std::string::npos) {
            size_t s = distPos + 10, e = screenBlock.find("\"", s);
            if (e != std::string::npos) {
                try {
                    currentScreen.animDistance = std::stof(screenBlock.substr(s, e - s));
                } catch(...) {}
            }
        }

        

        // 3. استخراج النصوص (Labels)
        size_t lPos = screenBlock.find("<Label ");
        while (lPos != std::string::npos) {
            size_t lEnd = screenBlock.find("/>", lPos); if (lEnd == std::string::npos) break;
            std::string lTag = screenBlock.substr(lPos, lEnd - lPos); LabelElement lbl; size_t p;
            lbl.animDistance = currentScreen.animDistance;
            lbl.animDuration = currentScreen.animDuration;
            p = lTag.find("name=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("name"), e=lTag.find("\"",s); if(e!=std::string::npos) lbl.name=lTag.substr(s,e-s); }
            p = lTag.find("text=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("text"), e=lTag.find("\"",s); if(e!=std::string::npos) lbl.text=lTag.substr(s,e-s); }
            p = lTag.find("position=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("position"), e=lTag.find("\"",s); if(e!=std::string::npos){ std::string pos=lTag.substr(s,e-s); size_t c=pos.find(","); if(c!=std::string::npos){ try{lbl.x=std::stof(pos.substr(0,c)); lbl.y=std::stof(pos.substr(c+1));}catch(...){} } } }
            p = lTag.find("size=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("size"), e=lTag.find("\"",s); if(e!=std::string::npos){ std::string sz=lTag.substr(s,e-s); size_t c=sz.find(","); if(c!=std::string::npos){ try{lbl.w=std::stof(sz.substr(0,c)); lbl.h=std::stof(sz.substr(c+1));}catch(...){} } } }
            p = lTag.find("foregroundColor=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("foregroundColor"), e=lTag.find("\"",s); if(e!=std::string::npos){ std::string hex=lTag.substr(s,e-s); if(hex.length()==7&&hex[0]=='#'){ try{lbl.r=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; lbl.g=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; lbl.b=std::stoi(hex.substr(5,2),nullptr,16)/255.0f;}catch(...){} } } }
            //p = lTag.find("color=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("color"), e=lTag.find("\"",s); if(e!=std::string::npos){ std::string hex=lTag.substr(s,e-s); try { if(hex.length()==7&&hex[0]=='#'){ lbl.bgR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; lbl.bgG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; lbl.bgB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f; lbl.bgA=1.0f; } else if(hex.length()==9&&hex[0]=='#'){ lbl.bgR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; lbl.bgG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; lbl.bgB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f; lbl.bgA=std::stoi(hex.substr(7,2),nullptr,16)/255.0f; } } catch(...) {} } }
            p = lTag.find("color=\""); if (p != std::string::npos) { 
                size_t s=p + GLM_ATTR_LEN("color"), e=lTag.find("\"",s); 
                if(e!=std::string::npos){ 
                    std::string cleanBg = "";
                    for (char c : lTag.substr(s,e-s)) { if (c != ' ' && c != '\t' && c != '\n' && c != '\r') cleanBg += c; }
                    std::vector<std::string> parts; size_t start = 0, end;
                    while ((end = cleanBg.find(";", start)) != std::string::npos) { parts.push_back(cleanBg.substr(start, end - start)); start = end + 1; }
                    parts.push_back(cleanBg.substr(start));
                    
                    if (parts.size() >= 2) {
                        std::string h1 = parts[0]; std::string h2 = parts[1]; std::string dir = (parts.size() >= 3) ? parts[2] : "vertical";
                        try {
                            if (h1.length() >= 7 && h1[0] == '#') { 
                                lbl.bgR = std::stoi(h1.substr(1,2),nullptr,16)/255.0f; lbl.bgG = std::stoi(h1.substr(3,2),nullptr,16)/255.0f; lbl.bgB = std::stoi(h1.substr(5,2),nullptr,16)/255.0f; lbl.bgA = (h1.length() >= 9) ? std::stoi(h1.substr(7,2),nullptr,16)/255.0f : 1.0f; 
                            }
                            if (h2.length() >= 7 && h2[0] == '#') { 
                                lbl.bgR2 = std::stoi(h2.substr(1,2),nullptr,16)/255.0f; lbl.bgG2 = std::stoi(h2.substr(3,2),nullptr,16)/255.0f; lbl.bgB2 = std::stoi(h2.substr(5,2),nullptr,16)/255.0f; lbl.bgA2 = (h2.length() >= 9) ? std::stoi(h2.substr(7,2),nullptr,16)/255.0f : 1.0f; 
                            }
                            if (dir == "horizontal") lbl.gradientMode = 2; else lbl.gradientMode = 1;
                        } catch(...) {}
                    } else {
                        try {
                            std::string h1 = parts[0];
                            if (h1.length() >= 7 && h1[0] == '#') { 
                                lbl.bgR = std::stoi(h1.substr(1,2),nullptr,16)/255.0f; lbl.bgG = std::stoi(h1.substr(3,2),nullptr,16)/255.0f; lbl.bgB = std::stoi(h1.substr(5,2),nullptr,16)/255.0f; lbl.bgA = (h1.length() >= 9) ? std::stoi(h1.substr(7,2),nullptr,16)/255.0f : 1.0f; 
                            }
                            lbl.gradientMode = 0;
                        } catch(...) {}
                    }
                } 
            }
            p = lTag.find("font=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("font"), e=lTag.find("\"",s); if(e!=std::string::npos){ std::string fStr=lTag.substr(s,e-s); size_t sc=fStr.find(";"); if(sc!=std::string::npos){ lbl.fontPath=fStr.substr(0,sc); try{lbl.fontSize=std::stoi(fStr.substr(sc+1));}catch(...){} } } }
            p = lTag.find("zPosition=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("zPosition"), e=lTag.find("\"",s); if(e!=std::string::npos){ try{lbl.z=std::stoi(lTag.substr(s,e-s));}catch(...){} } }
            
            // ✅ قراءة الأنيميشن الخاص بالـ Label
            p = lTag.find("Animation=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("Animation"), e=lTag.find("\"",s); if(e!=std::string::npos) { lbl.anim=lTag.substr(s,e-s); if (lbl.anim != "none" && !lbl.anim.empty()) { currentScreen.slideActive = true; currentScreen.slideStartTime = -1.0f; } } }
 
            // ✅ قراءة السرعة للـ Label
            p = lTag.find(" Speed=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN(" Speed"), e=lTag.find("\"",s); if(e!=std::string::npos) { try { float spd = std::stof(lTag.substr(s,e-s)); if (spd > 0.0f) { lbl.animDuration = 0.8f / spd; if (lbl.animDuration > currentScreen.animDuration) currentScreen.animDuration = lbl.animDuration; } } catch(...) {} } }

            // ✅ قراءة مسافة التحرك (distance) للنصوص
            p = lTag.find("distance=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("distance"), e=lTag.find("\"",s); if(e!=std::string::npos) { try { lbl.animDistance = std::stof(lTag.substr(s,e-s)); } catch(...) {} } }

            // ✅ قراءة خصائص الإطار (Border) - تم نقلها إلى هنا!
            p = lTag.find("border=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("border"), e=lTag.find("\"",s); if(e!=std::string::npos){ if(lTag.substr(s,e-s)=="1") lbl.hasBorder=true; } }
            p = lTag.find("borderWidth=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("borderWidth"), e=lTag.find("\"",s); if(e!=std::string::npos){ try{lbl.borderWidth=std::stof(lTag.substr(s,e-s));}catch(...){} } }
            p = lTag.find("borderColor=\""); if (p != std::string::npos) { 
                size_t s=p + GLM_ATTR_LEN("borderColor"), e=lTag.find("\"",s); 
                if(e!=std::string::npos){ 
                    std::string cleanStr = "";
                    for (char c : lTag.substr(s,e-s)) { if (c != ' ' && c != '\t' && c != '\n' && c != '\r') cleanStr += c; }
                    size_t semicolon = cleanStr.find(";");
                    if (semicolon != std::string::npos) {
                        std::string hex1 = cleanStr.substr(0, semicolon);
                        std::string remainder = cleanStr.substr(semicolon + 1);
                        size_t semicolon2 = remainder.find(";");
                        std::string hex2 = remainder;
                        std::string dir = "vertical";
                        if (semicolon2 != std::string::npos) { hex2 = remainder.substr(0, semicolon2); dir = remainder.substr(semicolon2 + 1); }
                        try {
                            if (hex1.length() >= 7 && hex1[0] == '#') { lbl.borderR = std::stoi(hex1.substr(1,2), nullptr, 16) / 255.0f; lbl.borderG = std::stoi(hex1.substr(3,2), nullptr, 16) / 255.0f; lbl.borderB = std::stoi(hex1.substr(5,2), nullptr, 16) / 255.0f; lbl.borderA = (hex1.length() >= 9) ? std::stoi(hex1.substr(7,2), nullptr, 16) / 255.0f : 1.0f; }
                            if (hex2.length() >= 7 && hex2[0] == '#') { lbl.borderR2 = std::stoi(hex2.substr(1,2), nullptr, 16) / 255.0f; lbl.borderG2 = std::stoi(hex2.substr(3,2), nullptr, 16) / 255.0f; lbl.borderB2 = std::stoi(hex2.substr(5,2), nullptr, 16) / 255.0f; lbl.borderA2 = (hex2.length() >= 9) ? std::stoi(hex2.substr(7,2), nullptr, 16) / 255.0f : 1.0f; }
                            lbl.borderGradientMode = (dir == "horizontal") ? 2 : 1;
                        } catch(...) {}
                    } else {
                        try {
                            if (cleanStr.length() >= 7 && cleanStr[0] == '#') { lbl.borderR = std::stoi(cleanStr.substr(1,2), nullptr, 16) / 255.0f; lbl.borderG = std::stoi(cleanStr.substr(3,2), nullptr, 16) / 255.0f; lbl.borderB = std::stoi(cleanStr.substr(5,2), nullptr, 16) / 255.0f; lbl.borderA = (cleanStr.length() >= 9) ? std::stoi(cleanStr.substr(7,2), nullptr, 16) / 255.0f : 1.0f; }
                            lbl.borderGradientMode = 0;
                        } catch(...) {}
                    }
                } 
            }
            
            // ✅ قراءة حواف دائرية (cornerDia)
            p = lTag.find("cornerDia=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("cornerDia"), e=lTag.find("\"",s); if(e!=std::string::npos){ try{lbl.cornerRadius=std::stof(lTag.substr(s,e-s))/2.0f;}catch(...){} } }
            // ✅ قراءة خصائص الأيقونة (fontIcon)
            p = lTag.find("fontIcon=\""); if (p != std::string::npos) { 
                size_t s=p + GLM_ATTR_LEN("fontIcon"), e=lTag.find("\"",s); 
                if(e!=std::string::npos){ 
                    std::string val = lTag.substr(s, e-s);
                    size_t c1 = val.find(";");
                    if(c1 != std::string::npos) {
                        lbl.iconFontPath = val.substr(0, c1);
                        size_t c2 = val.find(";", c1+1);
                        if(c2 != std::string::npos) {
                            try { lbl.iconFontSize = std::stoi(val.substr(c1+1, c2-c1-1)); } catch(...) {}
                            std::string hexStr = val.substr(c2+1);
                            try { lbl.iconCodepoint = std::stoul(hexStr, nullptr, 16); } catch(...) {}
                        }
                    }
                }
            }


            // ✅ قراءة محاذاة النص (text-halign)
            p = lTag.find("text-halign=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("text-halign"), e=lTag.find("\"",s); if(e!=std::string::npos) lbl.textAlign = lTag.substr(s,e-s); }
            
            // ✅ قراءة تفعيل السكرول للنصوص في Label
            p = lTag.find("scrolltext=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrolltext"), e=lTag.find("\"",s); if(e!=std::string::npos) try{lbl.scrollTextMode=std::stoi(lTag.substr(s,e-s));}catch(...){} }
            
            p = lTag.find("scrolltextshot=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrolltextshot"), e=lTag.find("\"",s); if(e!=std::string::npos) try{lbl.scrollTextShot=std::stoi(lTag.substr(s,e-s));}catch(...){} }

            p = lTag.find("scrolltextstyle=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrolltextstyle"), e=lTag.find("\"",s); if(e!=std::string::npos) try{lbl.scrollTextStyle=std::stoi(lTag.substr(s,e-s));}catch(...){} }
            
            p = lTag.find("scrolldelay=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrolldelay"), e=lTag.find("\"",s); if(e!=std::string::npos) try{lbl.scrollDelay=std::stof(lTag.substr(s,e-s));}catch(...){} }
            
            // ✅ قراءة موضع النص اليدوي (text-position)
            p = lTag.find("text-position=\""); if (p != std::string::npos) { 
                size_t s=p + GLM_ATTR_LEN("text-position"), e=lTag.find("\"",s); 
                if(e!=std::string::npos){ 
                    std::string posStr = lTag.substr(s,e-s); 
                    size_t c = posStr.find(",");
                    if(c != std::string::npos) {
                        try { 
                            lbl.manualTextX = std::stof(posStr.substr(0,c)); 
                            lbl.manualTextY = std::stof(posStr.substr(c+1)); 
                            lbl.useManualTextPos = true; 
                        } catch(...) {}
                    }
                }
            }
            // // 🚀 قراءة إعدادات التدرج اللوني للـ Label
            // p = lTag.find("gradient=\"");
            // if (p != std::string::npos) {
            //     size_t s=p + GLM_ATTR_LEN("gradient"), e=lTag.find("\"",s);
            //     if(e!=std::string::npos){
            //         std::string type = lTag.substr(s, e-s);
            //         if (type == "vertical") lbl.gradientMode = 1;
            //         else if (type == "horizontal") lbl.gradientMode = 2;
            //     }
            // }
            // p = lTag.find("gradientColor=\"");
            // if (p != std::string::npos) {
            //     size_t s=p + GLM_ATTR_LEN("gradientColor"), e=lTag.find("\"",s);
            //     if(e!=std::string::npos){
            //         std::string colors = lTag.substr(s, e-s);
            //         size_t comma = colors.find(",");
            //         if (comma != std::string::npos) {
            //             std::string hex1 = colors.substr(0, comma); std::string hex2 = colors.substr(comma+1);
            //             hex1.erase(std::remove_if(hex1.begin(), hex1.end(), ::isspace), hex1.end());
            //             hex2.erase(std::remove_if(hex2.begin(), hex2.end(), ::isspace), hex2.end());
            //             try {
            //                 if (hex1.length() == 7 && hex1[0] == '#') { lbl.bgR = std::stoi(hex1.substr(1,2),nullptr,16)/255.0f; lbl.bgG = std::stoi(hex1.substr(3,2),nullptr,16)/255.0f; lbl.bgB = std::stoi(hex1.substr(5,2),nullptr,16)/255.0f; lbl.bgA = 1.0f; }
            //                 else if (hex1.length() == 9 && hex1[0] == '#') { lbl.bgR = std::stoi(hex1.substr(1,2),nullptr,16)/255.0f; lbl.bgG = std::stoi(hex1.substr(3,2),nullptr,16)/255.0f; lbl.bgB = std::stoi(hex1.substr(5,2),nullptr,16)/255.0f; lbl.bgA = std::stoi(hex1.substr(7,2),nullptr,16)/255.0f; }
            //                 if (hex2.length() == 7 && hex2[0] == '#') { lbl.bgR2 = std::stoi(hex2.substr(1,2),nullptr,16)/255.0f; lbl.bgG2 = std::stoi(hex2.substr(3,2),nullptr,16)/255.0f; lbl.bgB2 = std::stoi(hex2.substr(5,2),nullptr,16)/255.0f; lbl.bgA2 = 1.0f; }
            //                 else if (hex2.length() == 9 && hex2[0] == '#') { lbl.bgR2 = std::stoi(hex2.substr(1,2),nullptr,16)/255.0f; lbl.bgG2 = std::stoi(hex2.substr(3,2),nullptr,16)/255.0f; lbl.bgB2 = std::stoi(hex2.substr(5,2),nullptr,16)/255.0f; lbl.bgA2 = std::stoi(hex2.substr(7,2),nullptr,16)/255.0f; }
            //             } catch(...) {}
            //         }
            //     }
            // }
            // ✅ إذا كان هناك نص، نرسم النص فقط. وإذا لم يكن هناك نص، نرسم الأيقونة فقط.
            if (!lbl.text.empty()) { 
                create_label_texture(lbl); 
            } else if (lbl.iconCodepoint != 0) { 
                create_icon_texture(lbl); 
            }
            
            // ✅ إضافة العنصر للرسم إذا كان يحتوي على خلفية، نص، إطار، أو أيقونة
            if (lbl.bgA > 0.0f || lbl.gradientMode > 0 || !lbl.text.empty() || lbl.hasBorder || lbl.iconCodepoint != 0) {
                currentScreen.labels.push_back(lbl);
            }
            lPos = screenBlock.find("<Label ", lEnd + 2);
        }
      
                
        // 4. استخراج الصور (Images/Pixmap)
        size_t iPos = screenBlock.find("<Pixmap ");
        while (iPos != std::string::npos) {
            size_t iEnd = screenBlock.find("/>", iPos); if (iEnd == std::string::npos) break;
            std::string iTag = screenBlock.substr(iPos, iEnd - iPos); ImageElement img; size_t p;
            img.animDistance = currentScreen.animDistance;
            img.animDuration = currentScreen.animDuration;
            p = iTag.find("name=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("name"), e=iTag.find("\"",s); if(e!=std::string::npos) img.name=iTag.substr(s,e-s); }

            // ✅ إصلاح قراءة المسار (التفريق بين المسار المطلق والنسبي بشكل صحيح)
            p = iTag.find("pixmap=\""); 
            if (p != std::string::npos) { 
                size_t s=p + GLM_ATTR_LEN("pixmap"), e=iTag.find("\"",s); 
                if(e!=std::string::npos) {
                    std::string pathVal = iTag.substr(s, e-s);
                    img.imagePath = (!pathVal.empty() && pathVal[0] == '/') ? pathVal : currentScreen.pluginBasePath + pathVal;
                }
            }
            p = iTag.find("position=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("position"), e=iTag.find("\"",s); if(e!=std::string::npos){ std::string pos=iTag.substr(s,e-s); size_t c=pos.find(","); if(c!=std::string::npos){ try{img.x=std::stof(pos.substr(0,c)); img.y=std::stof(pos.substr(c+1));}catch(...){} } } }
            p = iTag.find("size=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("size"), e=iTag.find("\"",s); if(e!=std::string::npos){ std::string sz=iTag.substr(s,e-s); size_t c=sz.find(","); if(c!=std::string::npos){ try{img.w=std::stof(sz.substr(0,c)); img.h=std::stof(sz.substr(c+1));}catch(...){} } } }
            p = iTag.find("zPosition=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("zPosition"), e=iTag.find("\"",s); if(e!=std::string::npos){ try{img.z=std::stoi(iTag.substr(s,e-s));}catch(...){} } }

            // ✅ قراءة الأنيميشن الخاص بالصور (Pixmaps) للتحكم بتثبيتها أو تحريكها
            p = iTag.find("Animation=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("Animation"), e=iTag.find("\"",s); if(e!=std::string::npos) { img.anim=iTag.substr(s,e-s); if (img.anim != "none" && !img.anim.empty()) { currentScreen.slideActive = true; currentScreen.slideStartTime = -1.0f; } } }
 
            // ✅ قراءة السرعة للصور
            p = iTag.find(" Speed=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN(" Speed"), e=iTag.find("\"",s); if(e!=std::string::npos) { try { float spd = std::stof(iTag.substr(s,e-s)); if (spd > 0.0f) { img.animDuration = 0.8f / spd; if (img.animDuration > currentScreen.animDuration) currentScreen.animDuration = img.animDuration; } } catch(...) {} } }
            
            // ✅ قراءة مسافة التحرك (distance) للصور
            p = iTag.find("distance=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("distance"), e=iTag.find("\"",s); if(e!=std::string::npos) { try { img.animDistance = std::stof(iTag.substr(s,e-s)); } catch(...) {} } }

            // ✅ قراءة زوايا دائرية للصور
            p = iTag.find("cornerDia=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("cornerDia"), e=iTag.find("\"",s); if(e!=std::string::npos){ try{img.cornerRadius=std::stof(iTag.substr(s,e-s))/2.0f;}catch(...){} } }

            // 🎨 Adaptive Background Color
            p = iTag.find("adaptivecolor=\"");
            if (p != std::string::npos) { size_t s2=p + GLM_ATTR_LEN("adaptivecolor"), e=iTag.find("\"",s2); if(e!=std::string::npos) img.adaptiveTint = (iTag.substr(s2,e-s2) == "1"); }

            p = iTag.find("adaptivesource=\"");
            if (p != std::string::npos) { size_t s2=p + GLM_ATTR_LEN("adaptivesource"), e=iTag.find("\"",s2); if(e!=std::string::npos) img.adaptiveSource = iTag.substr(s2,e-s2); }

            p = iTag.find("adaptivedim=\"");
            if (p != std::string::npos) { size_t s2=p + GLM_ATTR_LEN("adaptivedim"), e=iTag.find("\"",s2); if(e!=std::string::npos) try{img.adaptiveDim=std::stof(iTag.substr(s2,e-s2));}catch(...){} }

            p = iTag.find("adaptivespeed=\"");
            if (p != std::string::npos) { size_t s2=p + GLM_ATTR_LEN("adaptivespeed"), e=iTag.find("\"",s2); if(e!=std::string::npos) try{img.adaptiveSpeed=std::stof(iTag.substr(s2,e-s2));}catch(...){} }

            p = iTag.find("adaptivefallback=\"");
            if (p != std::string::npos) { size_t s2=p + GLM_ATTR_LEN("adaptivefallback"), e=iTag.find("\"",s2);
                if(e!=std::string::npos){ std::string hex=iTag.substr(s2,e-s2);
                    if(hex.length()>=7 && hex[0]=='#'){ try{
                        img.adpFallR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f;
                        img.adpFallG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f;
                        img.adpFallB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f;
                    }catch(...){} } } }

            if (!img.imagePath.empty()) {
                // 🎨 القناع التكيّفي لا يُخبز داخل بكسلات الباكدروب، بل يُرسم حيّاً كل إطار
                if (img.name == "mask_normal" && !img.adaptiveTint) {
                    currentScreen.maskPath = img.imagePath;   // 🎭 ملك الشاشة
                    std::lock_guard<std::mutex> qlock(queueMutex);
                    backdropMaskPath = img.imagePath;         // يبقى للتوافق فقط
                }
                load_image_texture(img);
                // ✅ جديد: إجبار إيقاف الشفافية لتسريع الواجهة حتى لو كانت الصورة PNG
                p = iTag.find("alphablend=\""); 
                if (p != std::string::npos) { 
                    size_t s=p + GLM_ATTR_LEN("alphablend"), e=iTag.find("\"",s); 
                    if(e!=std::string::npos && iTag.substr(s,e-s) == "0") {
                        img.hasAlpha = false; 
                    }
                }
                currentScreen.images.push_back(img);
            }
            iPos = screenBlock.find("<Pixmap ", iEnd + 2);
        }

        // ✅ 5. استخراج الـ Widgets (القوائم)
        size_t wPos = screenBlock.find("<Widget ");
        while (wPos != std::string::npos) {
            size_t wEnd = screenBlock.find("/>", wPos); if (wEnd == std::string::npos) break;
            std::string wTag = screenBlock.substr(wPos, wEnd - wPos); WidgetElement w; size_t p;
            w.animDistance = currentScreen.animDistance;
            w.animDuration = currentScreen.animDuration;
            
            p = wTag.find("name=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("name"), e=wTag.find("\"",s); if(e!=std::string::npos) w.name=wTag.substr(s,e-s); }
            p = wTag.find("position=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("position"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string pos=wTag.substr(s,e-s); size_t c=pos.find(","); if(c!=std::string::npos){ try{w.x=std::stof(pos.substr(0,c)); w.y=std::stof(pos.substr(c+1));}catch(...){} } } }
            p = wTag.find("size=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("size"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string sz=wTag.substr(s,e-s); size_t c=sz.find(","); if(c!=std::string::npos){ try{w.w=std::stof(sz.substr(0,c)); w.h=std::stof(sz.substr(c+1));}catch(...){} } } }
            p = wTag.find("foregroundColor=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("foregroundColor"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string hex=wTag.substr(s,e-s); if(hex.length()==7&&hex[0]=='#'){ try{w.fgR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; w.fgG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; w.fgB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f;}catch(...){} } } }
            p = wTag.find("foregroundColorSelected=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("foregroundColorSelected"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string hex=wTag.substr(s,e-s); if(hex.length()==7&&hex[0]=='#'){ try{w.fgSelR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; w.fgSelG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; w.fgSelB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f;}catch(...){} } } }
            p = wTag.find("backgroundColor=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("backgroundColor"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string hex=wTag.substr(s,e-s); if(hex.length()==7&&hex[0]=='#'){ try{w.bgR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; w.bgG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; w.bgB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f; w.bgA=1.0f;}catch(...){} } else if(hex.length()==9&&hex[0]=='#'){ try{w.bgR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; w.bgG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; w.bgB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f; w.bgA=std::stoi(hex.substr(7,2),nullptr,16)/255.0f;}catch(...){} } } }
            // ✅ قراءة خصائص الإطار والخلفية الجديدة للحاوية
            // ✅ قراءة خصائص الإطار والخلفية الجديدة للحاوية (تم الإصلاح بإضافة مسافة لمنع التداخل)
            p = wTag.find(" color=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN(" color"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string hex=wTag.substr(s,e-s); try { if(hex.length()==7&&hex[0]=='#'){ w.bgR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; w.bgG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; w.bgB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f; w.bgA=1.0f; } else if(hex.length()==9&&hex[0]=='#'){ w.bgR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; w.bgG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; w.bgB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f; w.bgA=std::stoi(hex.substr(7,2),nullptr,16)/255.0f; } } catch(...) {} } }
            p = wTag.find(" border=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN(" border"), e=wTag.find("\"",s); if(e!=std::string::npos){ if(wTag.substr(s,e-s)=="1") w.hasBorder=true; } }
            p = wTag.find(" borderWidth=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN(" borderWidth"), e=wTag.find("\"",s); if(e!=std::string::npos){ try{w.borderWidth=std::stof(wTag.substr(s,e-s));}catch(...){} } }
            p = wTag.find(" borderColor=\""); if (p != std::string::npos) { 
                size_t s=p + GLM_ATTR_LEN(" borderColor"), e=wTag.find("\"",s); 
                if(e!=std::string::npos){ 
                    std::string cleanStr = "";
                    for (char c : wTag.substr(s,e-s)) { if (c != ' ' && c != '\t' && c != '\n' && c != '\r') cleanStr += c; }
                    size_t semicolon = cleanStr.find(";");
                    if (semicolon != std::string::npos) {
                        std::string hex1 = cleanStr.substr(0, semicolon);
                        std::string remainder = cleanStr.substr(semicolon + 1);
                        size_t semicolon2 = remainder.find(";");
                        std::string hex2 = remainder;
                        std::string dir = "vertical";
                        if (semicolon2 != std::string::npos) { hex2 = remainder.substr(0, semicolon2); dir = remainder.substr(semicolon2 + 1); }
                        try {
                            if (hex1.length() >= 7 && hex1[0] == '#') { w.borderR = std::stoi(hex1.substr(1,2), nullptr, 16) / 255.0f; w.borderG = std::stoi(hex1.substr(3,2), nullptr, 16) / 255.0f; w.borderB = std::stoi(hex1.substr(5,2), nullptr, 16) / 255.0f; w.borderA = (hex1.length() >= 9) ? std::stoi(hex1.substr(7,2), nullptr, 16) / 255.0f : 1.0f; }
                            if (hex2.length() >= 7 && hex2[0] == '#') { w.borderR2 = std::stoi(hex2.substr(1,2), nullptr, 16) / 255.0f; w.borderG2 = std::stoi(hex2.substr(3,2), nullptr, 16) / 255.0f; w.borderB2 = std::stoi(hex2.substr(5,2), nullptr, 16) / 255.0f; w.borderA2 = (hex2.length() >= 9) ? std::stoi(hex2.substr(7,2), nullptr, 16) / 255.0f : 1.0f; }
                            w.borderGradientMode = (dir == "horizontal") ? 2 : 1;
                        } catch(...) {}
                    } else {
                        try {
                            if (cleanStr.length() >= 7 && cleanStr[0] == '#') { w.borderR = std::stoi(cleanStr.substr(1,2), nullptr, 16) / 255.0f; w.borderG = std::stoi(cleanStr.substr(3,2), nullptr, 16) / 255.0f; w.borderB = std::stoi(cleanStr.substr(5,2), nullptr, 16) / 255.0f; w.borderA = (cleanStr.length() >= 9) ? std::stoi(cleanStr.substr(7,2), nullptr, 16) / 255.0f : 1.0f; }
                            w.borderGradientMode = 0;
                        } catch(...) {}
                    }
                } 
            }
            p = wTag.find(" cornerDia=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN(" cornerDia"), e=wTag.find("\"",s); if(e!=std::string::npos){ try{w.cornerRadius=std::stof(wTag.substr(s,e-s))/2.0f;}catch(...){} } }
            p = wTag.find("zPosition=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("zPosition"), e=wTag.find("\"",s); if(e!=std::string::npos){ try{w.z=std::stoi(wTag.substr(s,e-s));}catch(...){} } }
            p = wTag.find("itemsize=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemsize"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string sz = wTag.substr(s,e-s); size_t c = sz.find(","); if(c != std::string::npos) { try { w.itemW = std::stof(sz.substr(0,c)); w.itemH = std::stof(sz.substr(c+1)); } catch(...) {} } } }
            p = wTag.find("itemgap=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemgap"), e=wTag.find("\"",s); if(e!=std::string::npos){ try{w.itemGap=std::stof(wTag.substr(s,e-s));}catch(...){} } }
            p = wTag.find("itemType=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemType"), e=wTag.find("\"",s); if(e!=std::string::npos) w.itemType=wTag.substr(s,e-s); }
            p = wTag.find("itemcolor=\""); 
            if (p != std::string::npos) { 
                size_t s=p + GLM_ATTR_LEN("itemcolor"), e=wTag.find("\"",s); 
                if(e!=std::string::npos){ 
                    std::string cleanStr = "";
                    for (char c : wTag.substr(s,e-s)) { if (c != ' ' && c != '\t' && c != '\n' && c != '\r') cleanStr += c; }
                    w.useItemColor = true;
                    size_t semicolon = cleanStr.find(";");
                    if (semicolon != std::string::npos) {
                        std::string hex1 = cleanStr.substr(0, semicolon);
                        std::string remainder = cleanStr.substr(semicolon + 1);
                        size_t semicolon2 = remainder.find(";");
                        std::string hex2 = remainder;
                        std::string dir = "vertical";
                        if (semicolon2 != std::string::npos) { hex2 = remainder.substr(0, semicolon2); dir = remainder.substr(semicolon2 + 1); }
                        try {
                            if (hex1.length() >= 7 && hex1[0] == '#') { w.itemBgR = std::stoi(hex1.substr(1,2), nullptr, 16) / 255.0f; w.itemBgG = std::stoi(hex1.substr(3,2), nullptr, 16) / 255.0f; w.itemBgB = std::stoi(hex1.substr(5,2), nullptr, 16) / 255.0f; w.itemBgA = (hex1.length() >= 9) ? std::stoi(hex1.substr(7,2), nullptr, 16) / 255.0f : 1.0f; }
                            if (hex2.length() >= 7 && hex2[0] == '#') { w.itemBgR2 = std::stoi(hex2.substr(1,2), nullptr, 16) / 255.0f; w.itemBgG2 = std::stoi(hex2.substr(3,2), nullptr, 16) / 255.0f; w.itemBgB2 = std::stoi(hex2.substr(5,2), nullptr, 16) / 255.0f; w.itemBgA2 = (hex2.length() >= 9) ? std::stoi(hex2.substr(7,2), nullptr, 16) / 255.0f : 1.0f; }
                            w.itemBgGradientMode = (dir == "horizontal") ? 2 : 1;
                        } catch(...) {}
                    } else {
                        try {
                            if (cleanStr.length() >= 7 && cleanStr[0] == '#') { w.itemBgR = std::stoi(cleanStr.substr(1,2), nullptr, 16) / 255.0f; w.itemBgG = std::stoi(cleanStr.substr(3,2), nullptr, 16) / 255.0f; w.itemBgB = std::stoi(cleanStr.substr(5,2), nullptr, 16) / 255.0f; w.itemBgA = (cleanStr.length() >= 9) ? std::stoi(cleanStr.substr(7,2), nullptr, 16) / 255.0f : 1.0f; }
                            w.itemBgGradientMode = 0;
                        } catch(...) {}
                    }
                } 
            }
            p = wTag.find("itemfont=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemfont"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string fStr=wTag.substr(s,e-s); size_t sc=fStr.find(";"); if(sc!=std::string::npos){ w.fontPath=fStr.substr(0,sc); try{w.fontSize=std::stoi(fStr.substr(sc+1));}catch(...){} } } }
            p = wTag.find("itemfontcolor=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemfontcolor"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string hex=wTag.substr(s,e-s); if(hex.length()==7&&hex[0]=='#'){ try{w.textR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; w.textG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; w.textB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f;}catch(...){} } } }
            p = wTag.find("itemtextoffsetY=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemtextoffsetY"), e=wTag.find("\"",s); if(e!=std::string::npos) try{w.textOffsetY=std::stof(wTag.substr(s,e-s));}catch(...){} }
            // ✅ قراءة خصائص محاذاة وإزاحة النص الجديدة
            p = wTag.find("itemtextAlign=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemtextAlign"), e=wTag.find("\"",s); if(e!=std::string::npos) w.itemTextAlign=wTag.substr(s,e-s); }
            
            p = wTag.find("itemtextoffsetX=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemtextoffsetX"), e=wTag.find("\"",s); if(e!=std::string::npos) try{w.itemTextOffsetX=std::stof(wTag.substr(s,e-s));}catch(...){} }
            // قراءة خصائص أيقونات المربعات
            p = wTag.find("font_icon=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("font_icon"), e=wTag.find("\"",s); if(e!=std::string::npos) w.iconFontPath = wTag.substr(s,e-s); }
            p = wTag.find("font_icon_size=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("font_icon_size"), e=wTag.find("\"",s); if(e!=std::string::npos) try{w.iconFontSize=std::stoi(wTag.substr(s,e-s));}catch(...){} }
            p = wTag.find("font_icon_position=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("font_icon_position"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string off=wTag.substr(s,e-s); size_t c=off.find(","); if(c!=std::string::npos){ try{w.iconOffsetX=std::stof(off.substr(0,c)); w.iconOffsetY=std::stof(off.substr(c+1));}catch(...){} } } }
            p = wTag.find("font_icon_forground=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("font_icon_forground"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string hex=wTag.substr(s,e-s); if(hex.length()==7&&hex[0]=='#'){ try{w.iconR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; w.iconG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; w.iconB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f;}catch(...){} } } }
            p = wTag.find("font_icon_forground_selected=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("font_icon_forground_selected"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string hex=wTag.substr(s,e-s); if(hex.length()==7&&hex[0]=='#'){ try{w.iconSelR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f; w.iconSelG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f; w.iconSelB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f;}catch(...){} } } }
            p = wTag.find("itemoffset=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemoffset"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string off=wTag.substr(s,e-s); size_t c=off.find(","); if(c!=std::string::npos){ try{w.itemOffsetX=std::stof(off.substr(0,c)); w.itemOffsetY=std::stof(off.substr(c+1));}catch(...){} } } }
            p = wTag.find("orientation=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("orientation"), e=wTag.find("\"",s); if(e!=std::string::npos) w.orientation=wTag.substr(s,e-s); }
            p = wTag.find("columns=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("columns"), e=wTag.find("\"",s); if(e!=std::string::npos){ try{w.gridColumns=std::stoi(wTag.substr(s,e-s));}catch(...){} } }
            p = wTag.find("CarouselSpeed=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("CarouselSpeed"), e=wTag.find("\"",s); if(e!=std::string::npos){ try{w.carouselDuration=std::stof(wTag.substr(s,e-s));}catch(...){} } }
            p = wTag.find("SelectionSpeed=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("SelectionSpeed"), e=wTag.find("\"",s); if(e!=std::string::npos){ try{w.selectionDuration=std::stof(wTag.substr(s,e-s));}catch(...){} } }
            // 🚀 قراءة خاصية حد الكاروسيل من الـ XML
            p = wTag.find("carousel_limit=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("carousel_limit"), e=wTag.find("\"",s); if(e!=std::string::npos) try{w.carouselLimit=std::stoi(wTag.substr(s,e-s));}catch(...){} }
            p = wTag.find("ZoominoutSpeed=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("ZoominoutSpeed"), e=wTag.find("\"",s); if(e!=std::string::npos){ try{w.zoomDuration=std::stof(wTag.substr(s,e-s));}catch(...){} } }
            // ✨ الهالة المضيئة
            p = wTag.find("glow=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("glow"), e=wTag.find("\"",s); if(e!=std::string::npos) w.glowEnabled=(wTag.substr(s,e-s)=="1"); }

            p = wTag.find("glowopacity=\"");
            if (p != std::string::npos) {
                size_t s=p + GLM_ATTR_LEN("glowopacity"), e=wTag.find("\"",s);
                if (e != std::string::npos) {
                    try {
                        float v = std::stof(wTag.substr(s, e - s));
                        if (v > 1.0f) v /= 100.0f;      // نقبل 0..1 أو 0..100
                        if (v < 0.0f) v = 0.0f;
                        if (v > 1.0f) v = 1.0f;
                        w.glowOpacity = v;
                    } catch(...) {}
                }
            }

            p = wTag.find("glowsize=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("glowsize"), e=wTag.find("\"",s); if(e!=std::string::npos) try{ w.glowSize=std::stof(wTag.substr(s,e-s)); }catch(...){} }

            {
                float ga = 1.0f;
                if (xml_attr_color(wTag, "glowcolor", w.glowR, w.glowG, w.glowB, ga)) w.glowHasColor = true;
            }

            // 🔍 الحالة الابتدائية للتكبير: ZoomActive="0" تفتح الشاشة بلا تكبير
            p = wTag.find("ZoomActive=\"");
            if (p != std::string::npos) {
                size_t s=p + GLM_ATTR_LEN("ZoomActive"), e=wTag.find("\"",s);
                if (e != std::string::npos) {
                    float on = (wTag.substr(s, e - s) == "1") ? 1.0f : 0.0f;
                    w.zoomShift = w.zoomShiftFrom = w.zoomShiftTo = on;
                    w.zoomShiftStart = -1.0f;     // بلا انتقال عند أول إطار
                }
            }

            // 🔍 مدة انتقال التكبير بالثواني (down/up)
            p = wTag.find("ZoomShiftSpeed=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("ZoomShiftSpeed"), e=wTag.find("\"",s); if(e!=std::string::npos) try{ w.zoomShiftDur=std::stof(wTag.substr(s,e-s)); }catch(...){} }

            p = wTag.find("itemImageFit=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemImageFit"), e=wTag.find("\"",s); if(e!=std::string::npos) w.itemFitCover=(wTag.substr(s,e-s)=="cover"); }

            p = wTag.find("ZoomStatus=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("ZoomStatus"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string val=wTag.substr(s,e-s); w.zoomEnabled=(val=="1"); } }
            p = wTag.find("zoomSize=\""); if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("zoomSize"), e=wTag.find("\"",s); if(e!=std::string::npos){ try{w.zoomFactor=std::stof(wTag.substr(s,e-s));}catch(...){} } }
            p = wTag.find("selectionpixmap=\""); if (p != std::string::npos) { 
                size_t s=p + GLM_ATTR_LEN("selectionpixmap"), e=wTag.find("\"",s); 
                if(e!=std::string::npos) {
                    w.selectionPixmapPath = wTag.substr(s,e-s);
                    w.showSelection = true; // ✅ تفعيل الفوكس تلقائياً لأن هذا العنصر قائمة
                    
                    // ✅ 1. التحقق إذا كان النمط هو إطار "border"
                    if (w.selectionPixmapPath == "border") {
                        w.selectionIsBorder = true;
                        // قراءة حجم الإطار
                        size_t bp = wTag.find("selectionbordersize=\"");
                        if (bp != std::string::npos) {
                            size_t bs = bp + 21, be = wTag.find("\"", bs);
                            if (be != std::string::npos) try { w.selBorderSize = std::stof(wTag.substr(bs, be - bs)); } catch(...) {}
                        }
                        // قراءة لون الإطار (عادي أو متدرج)
                        size_t cp = wTag.find("selectionbordercolor=\"");
                        if (cp != std::string::npos) {
                            size_t cs = cp + 22, ce = wTag.find("\"", cs);
                            if (ce != std::string::npos) {
                                std::string colorStr = wTag.substr(cs, ce - cs);
                                std::string cleanStr = "";
                                for (char c : colorStr) {
                                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') cleanStr += c;
                                }
                                std::vector<std::string> parts;
                                size_t start = 0, end;
                                while ((end = cleanStr.find(";", start)) != std::string::npos) {
                                    parts.push_back(cleanStr.substr(start, end - start));
                                    start = end + 1;
                                }
                                parts.push_back(cleanStr.substr(start));

                                if (parts.size() >= 2) {
                                    std::string hex1 = parts[0];
                                    std::string hex2 = parts[1];
                                    std::string dir = (parts.size() >= 3) ? parts[2] : "vertical";
                                    
                                    try {
                                        if (hex1.length() >= 7 && hex1[0] == '#') {
                                            unsigned int col1 = std::stoul(hex1.substr(1), nullptr, 16);
                                            if (hex1.length() == 7) {
                                                w.selBorderR = ((col1>>16)&0xFF)/255.0f; w.selBorderG = ((col1>>8)&0xFF)/255.0f; w.selBorderB = (col1&0xFF)/255.0f; w.selBorderA = 1.0f;
                                            } else {
                                                w.selBorderR = ((col1>>24)&0xFF)/255.0f; w.selBorderG = ((col1>>16)&0xFF)/255.0f; w.selBorderB = ((col1>>8)&0xFF)/255.0f; w.selBorderA = (col1&0xFF)/255.0f;
                                            }
                                        }
                                        if (hex2.length() >= 7 && hex2[0] == '#') {
                                            unsigned int col2 = std::stoul(hex2.substr(1), nullptr, 16);
                                            if (hex2.length() == 7) {
                                                w.selBorderR2 = ((col2>>16)&0xFF)/255.0f; w.selBorderG2 = ((col2>>8)&0xFF)/255.0f; w.selBorderB2 = (col2&0xFF)/255.0f; w.selBorderA2 = 1.0f;
                                            } else {
                                                w.selBorderR2 = ((col2>>24)&0xFF)/255.0f; w.selBorderG2 = ((col2>>16)&0xFF)/255.0f; w.selBorderB2 = ((col2>>8)&0xFF)/255.0f; w.selBorderA2 = (col2&0xFF)/255.0f;
                                            }
                                        }
                                        if (dir == "horizontal") w.selBorderGradientMode = 2;
                                        else w.selBorderGradientMode = 1;
                                    } catch(...) {}
                                } else {
                                    try {
                                        std::string hex1 = parts[0];
                                        if (hex1.length() >= 7 && hex1[0] == '#') {
                                            unsigned int col = std::stoul(hex1.substr(1), nullptr, 16);
                                            if (hex1.length() == 7) {
                                                w.selBorderR = ((col>>16)&0xFF)/255.0f; w.selBorderG = ((col>>8)&0xFF)/255.0f; w.selBorderB = (col&0xFF)/255.0f; w.selBorderA = 1.0f;
                                            } else {
                                                w.selBorderR = ((col>>24)&0xFF)/255.0f; w.selBorderG = ((col>>16)&0xFF)/255.0f; w.selBorderB = ((col>>8)&0xFF)/255.0f; w.selBorderA = (col&0xFF)/255.0f;
                                            }
                                        }
                                        w.selBorderGradientMode = 0;
                                    } catch(...) {}
                                }
                            }
                        }
                    }
                    // ✅ 2. التحقق إذا بدأ بـ '#' فهو لون مصمت أو متدرج (مع دعم الاتجاه)
                    else if (w.selectionPixmapPath.length() > 1 && w.selectionPixmapPath[0] == '#') {
                        w.selectionIsColor = true;
                        std::string cleanStr = "";
                        // إزالة الفراغات لضمان عدم حدوث أخطاء في القراءة
                        for (char c : w.selectionPixmapPath) {
                            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') cleanStr += c;
                        }
                        
                        // تقسيم النص بناءً على الفاصلة المنقوطة ';'
                        std::vector<std::string> parts;
                        size_t start = 0, end;
                        while ((end = cleanStr.find(";", start)) != std::string::npos) {
                            parts.push_back(cleanStr.substr(start, end - start));
                            start = end + 1;
                        }
                        parts.push_back(cleanStr.substr(start));

                        if (parts.size() >= 2) {
                            std::string hex1 = parts[0];
                            std::string hex2 = parts[1];
                            std::string dir = (parts.size() >= 3) ? parts[2] : "vertical"; // الاتجاه الافتراضي عمودي

                            try {
                                if (hex1.length() >= 7 && hex1[0] == '#') {
                                    unsigned int col1 = std::stoul(hex1.substr(1), nullptr, 16);
                                    if (hex1.length() == 7) {
                                        w.selR = ((col1>>16)&0xFF)/255.0f; w.selG = ((col1>>8)&0xFF)/255.0f; w.selB = (col1&0xFF)/255.0f; w.selA = 1.0f;
                                    } else {
                                        w.selR = ((col1>>24)&0xFF)/255.0f; w.selG = ((col1>>16)&0xFF)/255.0f; w.selB = ((col1>>8)&0xFF)/255.0f; w.selA = (col1&0xFF)/255.0f;
                                    }
                                }
                                if (hex2.length() >= 7 && hex2[0] == '#') {
                                    unsigned int col2 = std::stoul(hex2.substr(1), nullptr, 16);
                                    if (hex2.length() == 7) {
                                        w.selR2 = ((col2>>16)&0xFF)/255.0f; w.selG2 = ((col2>>8)&0xFF)/255.0f; w.selB2 = (col2&0xFF)/255.0f; w.selA2 = 1.0f;
                                    } else {
                                        w.selR2 = ((col2>>24)&0xFF)/255.0f; w.selG2 = ((col2>>16)&0xFF)/255.0f; w.selB2 = ((col2>>8)&0xFF)/255.0f; w.selA2 = (col2&0xFF)/255.0f;
                                    }
                                }
                                // 🚀 تفعيل التدرج بناءً على الاتجاه المكتوب في الـ XML
                                if (dir == "horizontal") w.selGradientMode = 2;
                                else w.selGradientMode = 1;
                                
                            } catch(...) {}
                        } else {
                            // إذا تم تمرير لون واحد فقط بدون فاصلة
                            try {
                                std::string hex1 = parts[0];
                                if (hex1.length() >= 7 && hex1[0] == '#') {
                                    unsigned int col = std::stoul(hex1.substr(1), nullptr, 16);
                                    if (hex1.length() == 7) {
                                        w.selR = ((col>>16)&0xFF)/255.0f; w.selG = ((col>>8)&0xFF)/255.0f; w.selB = (col&0xFF)/255.0f; w.selA = 1.0f;
                                    } else {
                                        w.selR = ((col>>24)&0xFF)/255.0f; w.selG = ((col>>16)&0xFF)/255.0f; w.selB = ((col>>8)&0xFF)/255.0f; w.selA = (col&0xFF)/255.0f;
                                    }
                                }
                                w.selGradientMode = 0; // إيقاف التدرج
                            } catch(...) {}
                        }
                    } else {
                        // ✅ 3. إنه مسار صورة، قم بتحميلها
                        std::string imgCandidate = (!w.selectionPixmapPath.empty() && w.selectionPixmapPath[0] == '/') ? w.selectionPixmapPath : currentScreen.pluginBasePath + w.selectionPixmapPath;
                        int imgW, imgH, imgComp;
                        unsigned char* imgData = stbi_load(imgCandidate.c_str(), &imgW, &imgH, &imgComp, 4);
                        if (imgData) {
                            glGenTextures(1, &w.selectionTexId);
                            glBindTexture(GL_TEXTURE_2D, w.selectionTexId);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, imgW, imgH, 0, GL_RGBA, GL_UNSIGNED_BYTE, imgData);
                            w.selTexW = imgW; w.selTexH = imgH;
                            w.selectionLoaded = true;
                            stbi_image_free(imgData);
                        }
                    }
                }
            }

            // ✅ قراءة قيمة دوران حواف العناصر (البوسترات) داخل الـ Widget
            p = wTag.find("itemcornerDia=\"");
            if (p != std::string::npos) {
                size_t s = p + 15, e = wTag.find("\"", s);
                if (e != std::string::npos) try { w.itemCornerRadius = std::stof(wTag.substr(s, e - s)) / 2.0f; } catch(...) {}
            }
            
            // ✅ قراءة قيمة دوران حواف التحديد
            p = wTag.find("selectionpixmapcornerDia=\"");
            if (p != std::string::npos) {
                size_t s = p + 26, e = wTag.find("\"", s);
                if (e != std::string::npos) try { w.selectionCornerRadius = std::stof(wTag.substr(s, e - s)) / 2.0f; } catch(...) {}
            }
            // ✅ قراءة مسافة التحديد (Padding) من الـ XML
            p = wTag.find("selectionPadding=\"");
            if (p != std::string::npos) {
                size_t s = p + 18, e = wTag.find("\"", s);
                if (e != std::string::npos) try { w.selectionPadding = std::stof(wTag.substr(s, e - s)); } catch(...) {}
            }

            // ==========================================================================
            // 📏 شريط التمرير (Scrollbar)
            // ==========================================================================
            p = wTag.find("scrollbar=\"");
            if (p != std::string::npos) {
                size_t s=p + GLM_ATTR_LEN("scrollbar"), e=wTag.find("\"",s);
                if (e != std::string::npos) {
                    std::string mode = wTag.substr(s, e - s);
                    for (char& c : mode) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                    if      (mode.find("never")  != std::string::npos) w.scrollbarMode = 0;
                    else if (mode.find("alway")  != std::string::npos) w.scrollbarMode = 2;  // ShowAlways / ShowAlwayse
                    else if (mode.find("demand") != std::string::npos) w.scrollbarMode = 1;  // ShowOnDemand / SowOnDemand
                    else if (mode == "1") w.scrollbarMode = 1;
                    else if (mode == "2") w.scrollbarMode = 2;
                    else w.scrollbarMode = 0;
                }
            }

            p = wTag.find("scrollbarSize=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrollbarSize"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string v=wTag.substr(s,e-s); size_t c=v.find(","); if(c!=std::string::npos){ try{ w.scrollbarW=std::stof(v.substr(0,c)); w.scrollbarH=std::stof(v.substr(c+1)); }catch(...){} } } }

            p = wTag.find("scrollbarOffset=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrollbarOffset"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string v=wTag.substr(s,e-s); size_t c=v.find(","); if(c!=std::string::npos){ try{ w.scrollbarOffX=std::stof(v.substr(0,c)); w.scrollbarOffY=std::stof(v.substr(c+1)); }catch(...){} } } }

            p = wTag.find("scrollbarCornerRadius=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrollbarCornerRadius"), e=wTag.find("\"",s); if(e!=std::string::npos) try{ w.scrollbarRadius=std::stof(wTag.substr(s,e-s)); }catch(...){} }

            p = wTag.find("sliderSize=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("sliderSize"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string v=wTag.substr(s,e-s); size_t c=v.find(","); if(c!=std::string::npos){ try{ w.sliderW=std::stof(v.substr(0,c)); w.sliderH=std::stof(v.substr(c+1)); }catch(...){} } } }

            p = wTag.find("sliderCornerRadius=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("sliderCornerRadius"), e=wTag.find("\"",s); if(e!=std::string::npos) try{ w.sliderRadius=std::stof(wTag.substr(s,e-s)); }catch(...){} }

            p = wTag.find("sliderAnim=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("sliderAnim"), e=wTag.find("\"",s); if(e!=std::string::npos) w.sliderAnim=(wTag.substr(s,e-s)=="1"); }

            xml_attr_color(wTag, "scrollbarColor", w.scrollbarR, w.scrollbarG, w.scrollbarB, w.scrollbarA);
            xml_attr_color(wTag, "sliderColor",    w.sliderR,    w.sliderG,    w.sliderB,    w.sliderA);

            // ==========================================================================
            // 📊 شريط التقدّم المدمج (Native Progress Bar)
            // ==========================================================================
            p = wTag.find("progressBar=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("progressBar"), e=wTag.find("\"",s); if(e!=std::string::npos) w.progressEnabled=(wTag.substr(s,e-s)=="1"); }

            // progRect="x,y,w,h" — نسبةً لصندوق العنصر
            p = wTag.find("progRect=\"");
            if (p != std::string::npos) {
                size_t s=p + GLM_ATTR_LEN("progRect"), e=wTag.find("\"",s);
                if (e != std::string::npos) {
                    std::string rectStr = wTag.substr(s, e - s);
                    float vals[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                    int n = 0;
                    std::stringstream rs(rectStr);
                    std::string tok;
                    while (n < 4 && std::getline(rs, tok, ',')) { try { vals[n] = std::stof(tok); } catch(...) {} n++; }
                    if (n >= 4) { w.progX = vals[0]; w.progY = vals[1]; w.progW = vals[2]; w.progH = vals[3]; }
                }
            }

            p = wTag.find("progCornerDia=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("progCornerDia"), e=wTag.find("\"",s); if(e!=std::string::npos) try{ w.progCornerRadius = std::stof(wTag.substr(s,e-s)) / 2.0f; }catch(...){} }

            xml_attr_color(wTag, "progTrackColor",         w.progTrackR,    w.progTrackG,    w.progTrackB,    w.progTrackA);
            xml_attr_color(wTag, "progFillColor",          w.progFillR,     w.progFillG,     w.progFillB,     w.progFillA);
            xml_attr_color(wTag, "progTrackColorSelected", w.progTrackSelR, w.progTrackSelG, w.progTrackSelB, w.progTrackSelA);
            xml_attr_color(wTag, "progFillColorSelected",  w.progFillSelR,  w.progFillSelG,  w.progFillSelB,  w.progFillSelA);

            // ✅ قراءة خاصية تفعيل انيميشن إطار التحديد
            p = wTag.find("selectionpixmapAnim=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("selectionpixmapAnim"), e=wTag.find("\"",s); if(e!=std::string::npos) w.selectionpixmapAnim=(wTag.substr(s,e-s)=="1"); }
            
            // قراءة تفعيل الانعكاس من الـ XML
            p = wTag.find("reflection=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("reflection"), e=wTag.find("\"",s); if(e!=std::string::npos) w.hasReflection=(wTag.substr(s,e-s)=="1"); }
            
            // ✅ قراءة تفعيل السكرول للنصوص (scrolltext)
            p = wTag.find("scrolltext=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrolltext"), e=wTag.find("\"",s); if(e!=std::string::npos) try{w.scrollTextMode=std::stoi(wTag.substr(s,e-s));}catch(...){} }
            
            p = wTag.find("scrolltextshot=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrolltextshot"), e=wTag.find("\"",s); if(e!=std::string::npos) try{w.scrollTextShot=std::stoi(wTag.substr(s,e-s));}catch(...){} }

            p = wTag.find("scrolltextstyle=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrolltextstyle"), e=wTag.find("\"",s); if(e!=std::string::npos) try{w.scrollTextStyle=std::stoi(wTag.substr(s,e-s));}catch(...){} }
            
            p = wTag.find("scrolldelay=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("scrolldelay"), e=wTag.find("\"",s); if(e!=std::string::npos) try{w.scrollDelay=std::stof(wTag.substr(s,e-s));}catch(...){} }
            
            // ✅ قراءة القياسات المخصصة الجديدة
            // ✅ ظل/حدّ النص: itemtextshadow="1|2" + itemtextshadowcolor + itemtextshadowoffset
            p = wTag.find("itemtextshadow=\"");
            if (p != std::string::npos) { size_t s2=p + GLM_ATTR_LEN("itemtextshadow"), e=wTag.find("\"",s2); if(e!=std::string::npos) try{w.textShadowMode=std::stoi(wTag.substr(s2,e-s2));}catch(...){} }

            p = wTag.find("itemtextshadowcolor=\"");
            if (p != std::string::npos) { size_t s2=p + GLM_ATTR_LEN("itemtextshadowcolor"), e=wTag.find("\"",s2);
                if(e!=std::string::npos){ std::string hex=wTag.substr(s2,e-s2);
                    if(hex.length()>=7 && hex[0]=='#'){ try{
                        w.textShadowR=std::stoi(hex.substr(1,2),nullptr,16)/255.0f;
                        w.textShadowG=std::stoi(hex.substr(3,2),nullptr,16)/255.0f;
                        w.textShadowB=std::stoi(hex.substr(5,2),nullptr,16)/255.0f;
                        w.textShadowA=(hex.length()>=9)?std::stoi(hex.substr(7,2),nullptr,16)/255.0f:1.0f;
                    }catch(...){} } } }

            p = wTag.find("itemtextshadowoffset=\"");
            if (p != std::string::npos) { size_t s2=p + GLM_ATTR_LEN("itemtextshadowoffset"), e=wTag.find("\"",s2);
                if(e!=std::string::npos){ std::string off=wTag.substr(s2,e-s2); size_t c=off.find(",");
                    if(c!=std::string::npos){ try{w.textShadowOffX=std::stof(off.substr(0,c)); w.textShadowOffY=std::stof(off.substr(c+1));}catch(...){} } } }

            p = wTag.find("itemtextmaxW=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("itemtextmaxW"), e=wTag.find("\"",s); if(e!=std::string::npos) try{w.itemTextMaxW=std::stof(wTag.substr(s,e-s));}catch(...){} }

            p = wTag.find("selectionSize=\""); 
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("selectionSize"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string sz=wTag.substr(s,e-s); size_t c=sz.find(","); if(c!=std::string::npos){ try{w.selectionW=std::stof(sz.substr(0,c)); w.selectionH=std::stof(sz.substr(c+1));}catch(...){} } } }

            p = wTag.find("selectionOffset=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("selectionOffset"), e=wTag.find("\"",s); if(e!=std::string::npos){ std::string off=wTag.substr(s,e-s); size_t c=off.find(","); if(c!=std::string::npos){ try{w.selectionOffsetX=std::stof(off.substr(0,c)); w.selectionOffsetY=std::stof(off.substr(c+1));}catch(...){} } } }


            // 🚀 قراءة إعدادات التدرج اللوني للـ Widget
            p = wTag.find("gradient=\"");
            if (p != std::string::npos) {
                size_t s=p + GLM_ATTR_LEN("gradient"), e=wTag.find("\"",s);
                if(e!=std::string::npos){
                    std::string type = wTag.substr(s, e-s);
                    if (type == "vertical") w.gradientMode = 1;
                    else if (type == "horizontal") w.gradientMode = 2;
                }
            }
            p = wTag.find("gradientColor=\"");
            if (p != std::string::npos) {
                size_t s=p + GLM_ATTR_LEN("gradientColor"), e=wTag.find("\"",s);
                if(e!=std::string::npos){
                    std::string colors = wTag.substr(s, e-s);
                    size_t comma = colors.find(",");
                    if (comma != std::string::npos) {
                        std::string hex1 = colors.substr(0, comma); std::string hex2 = colors.substr(comma+1);
                        hex1.erase(std::remove_if(hex1.begin(), hex1.end(), ::isspace), hex1.end());
                        hex2.erase(std::remove_if(hex2.begin(), hex2.end(), ::isspace), hex2.end());
                        try {
                            if (hex1.length() == 7 && hex1[0] == '#') { w.bgR = std::stoi(hex1.substr(1,2),nullptr,16)/255.0f; w.bgG = std::stoi(hex1.substr(3,2),nullptr,16)/255.0f; w.bgB = std::stoi(hex1.substr(5,2),nullptr,16)/255.0f; w.bgA = 1.0f; }
                            else if (hex1.length() == 9 && hex1[0] == '#') { w.bgR = std::stoi(hex1.substr(1,2),nullptr,16)/255.0f; w.bgG = std::stoi(hex1.substr(3,2),nullptr,16)/255.0f; w.bgB = std::stoi(hex1.substr(5,2),nullptr,16)/255.0f; w.bgA = std::stoi(hex1.substr(7,2),nullptr,16)/255.0f; }
                            if (hex2.length() == 7 && hex2[0] == '#') { w.bgR2 = std::stoi(hex2.substr(1,2),nullptr,16)/255.0f; w.bgG2 = std::stoi(hex2.substr(3,2),nullptr,16)/255.0f; w.bgB2 = std::stoi(hex2.substr(5,2),nullptr,16)/255.0f; w.bgA2 = 1.0f; }
                            else if (hex2.length() == 9 && hex2[0] == '#') { w.bgR2 = std::stoi(hex2.substr(1,2),nullptr,16)/255.0f; w.bgG2 = std::stoi(hex2.substr(3,2),nullptr,16)/255.0f; w.bgB2 = std::stoi(hex2.substr(5,2),nullptr,16)/255.0f; w.bgA2 = std::stoi(hex2.substr(7,2),nullptr,16)/255.0f; }
                        } catch(...) {}
                    }
                }
            } 
            //if (p != std::string::npos) { size_t s=p+17, e=wTag.find("\"",s); if(e!=std::string::npos){ std::string off=wTag.substr(s,e-s); size_t c=off.find(","); if(c!=std::string::npos){ try{w.selectionOffsetX=std::stof(off.substr(0,c)); w.selectionOffsetY=std::stof(off.substr(c+1));}catch(...){} } } }
            
            
            // ✅ قراءة الأنيميشن الخاص بالـ Widget
            p = wTag.find("Animation=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("Animation"), e=wTag.find("\"",s); if(e!=std::string::npos) { w.anim=wTag.substr(s,e-s); if (w.anim != "none" && !w.anim.empty()) { currentScreen.slideActive = true; currentScreen.slideStartTime = -1.0f; } } }
 
            // ✅ قراءة السرعة للـ Widget
            p = wTag.find(" Speed=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN(" Speed"), e=wTag.find("\"",s); if(e!=std::string::npos) { try { float spd = std::stof(wTag.substr(s,e-s)); if (spd > 0.0f) { w.animDuration = 0.8f / spd; if (w.animDuration > currentScreen.animDuration) currentScreen.animDuration = w.animDuration; } } catch(...) {} } }
            
            // ✅ قراءة مسافة التحرك (distance) للويدجت
            p = wTag.find("distance=\"");
            if (p != std::string::npos) { size_t s=p + GLM_ATTR_LEN("distance"), e=wTag.find("\"",s); if(e!=std::string::npos) { try { w.animDistance = std::stof(wTag.substr(s,e-s)); } catch(...) {} } }
            
            currentScreen.widgets.push_back(w);
            wPos = screenBlock.find("<Widget ", wEnd + 2);
        }
        // 🚀 تحميل صور السپينر مرة واحدة في الذاكرة لتكون جاهزة دائماً
        if (texLoadingBack == 0) {
            int w, h, c;
            unsigned char* d1 = stbi_load((currentScreen.pluginBasePath + "spinner/loading-back.png").c_str(), &w, &h, &c, 4);
            if (d1) { glGenTextures(1, &texLoadingBack); glBindTexture(GL_TEXTURE_2D, texLoadingBack); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d1); stbi_image_free(d1); }
            unsigned char* d2 = stbi_load((currentScreen.pluginBasePath + "spinner/loading-spinner.png").c_str(), &w, &h, &c, 4);
            if (d2) { glGenTextures(1, &texLoadingSpinner); glBindTexture(GL_TEXTURE_2D, texLoadingSpinner); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d2); stbi_image_free(d2); }
        }
        GLM_LOG("[GLM-DEBUG] ==========================================\n");
        GLM_LOG("[GLM-DEBUG] 2. XML LOADING SUMMARY\n");
        GLM_LOG("[GLM-DEBUG] XML Path: %s\n", xmlPath);
        GLM_LOG("[GLM-DEBUG] Screen Name: %s\n", screenName);
        GLM_LOG("[GLM-DEBUG] Parsed Widgets: %zu\n", currentScreen.widgets.size());
        GLM_LOG("[GLM-DEBUG] Parsed Images: %zu\n", currentScreen.images.size());
        GLM_LOG("[GLM-DEBUG] Parsed Labels: %zu\n", currentScreen.labels.size());
        if (currentScreen.widgets.size() == 0) {
            GLM_LOG("[GLM-DEBUG] WARNING: NO WIDGETS FOUND! Check XML layout name.\n");
        }
        GLM_LOG("[GLM-DEBUG] ==========================================\n");

        // 🎬 تطبيق الفيد المؤجَّل — آخر خطوة داخل نفس المهمة.
        // في الأعلى صفّرنا screenFadeState، وهنا نعيد تسليحه إن طلبته بايثون
        // قبل التحميل. لأن هذا يجري داخل المهمة نفسها، يستحيل أن يُرسم إطار
        // بين بناء الشاشة وبداية الفيد.
        int pendingFade = pending_screen_fade_type.exchange(0);
        if (pendingFade != 0) {
            currentScreen.screenFadeState     = pendingFade;
            currentScreen.screenFadeStartTime = -1.0f;   // يُضبط عند أول إطار
            float pd = pending_screen_fade_duration.load();
            if (pd > 0.0f) currentScreen.screenFadeDuration = pd;
            GLM_LOG("[GLM-DEBUG] pending screen fade applied: type=%d duration=%.2f\n", pendingFade, pd);
        }
    }

    bool ensure_static_scene_fbo(int w, int h) {
        if (w <= 0 || h <= 0) return false;

        if (staticSceneFbo != 0 && staticSceneTex != 0 && staticSceneW == w && staticSceneH == h) {
            return true;
        }

        if (staticSceneTex != 0) { glDeleteTextures(1, &staticSceneTex); staticSceneTex = 0; }
        if (staticSceneFbo != 0) { glDeleteFramebuffers(1, &staticSceneFbo); staticSceneFbo = 0; }

        glGenTextures(1, &staticSceneTex);
        glBindTexture(GL_TEXTURE_2D, staticSceneTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glGenFramebuffers(1, &staticSceneFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, staticSceneFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, staticSceneTex, 0);

        bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo);

        if (!ok) {
            if (staticSceneTex != 0) { glDeleteTextures(1, &staticSceneTex); staticSceneTex = 0; }
            if (staticSceneFbo != 0) { glDeleteFramebuffers(1, &staticSceneFbo); staticSceneFbo = 0; }
            staticSceneW = 0; staticSceneH = 0; staticSceneValid = false; staticSceneDirty.store(true);
            return false;
        }

        staticSceneW = w;
        staticSceneH = h;
        staticSceneValid = false;
        staticSceneDirty.store(true);
        return true;
    }

    // ==================================================================================
    // 🔊 GLM VOLUME OSD - محرك رسم شريط الصوت
    // ==================================================================================

    // توليد صورة نص (أرقام النسبة) مع تخزينها في الكاش العالمي
    GLuint glm_volume_text_texture(const std::string& fontPathIn, int fontSize, const std::string& text, float& outW, float& outH) {
        if (text.empty() || fontSize <= 0) return 0;
        std::string fontPath = (!fontPathIn.empty() && fontPathIn[0] == '/')
                             ? fontPathIn : (currentScreen.pluginBasePath + fontPathIn);
        std::string key = "glmvol_txt_" + fontPath + "_" + std::to_string(fontSize) + "_" + text;
        if (const CachedText* c = globalTextCache.get(key)) {
            outW = (float)c->w; outH = (float)c->h;
            return c->texId;
        }

        FT_Face face = get_ft_face(fontPath);
        if (!face) return 0;
        FT_Set_Pixel_Sizes(face, 0, fontSize);

        int asc  = face->size->metrics.ascender >> 6;
        int desc = std::abs(face->size->metrics.descender >> 6);
        int th = asc + desc;
        if (th <= 0) { th = fontSize + 4; asc = fontSize; }
        int baseline = asc;

        int tw = 0;
        {
            const unsigned char* p = (const unsigned char*)text.c_str();
            while (*p) {
                uint32_t cp = next_utf8_codepoint(p);
                if (!cp) break;
                if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT)) continue;
                tw += (int)(face->glyph->advance.x >> 6);
            }
        }
        tw += 4;
        if (tw <= 4 || th <= 0) return 0;

        unsigned char* bmp = new (std::nothrow) unsigned char[(size_t)tw * th * 4]();
        if (!bmp) return 0;
        int penX = 2;
        {
            const unsigned char* p = (const unsigned char*)text.c_str();
            while (*p) {
                uint32_t cp = next_utf8_codepoint(p);
                if (!cp) break;
                if (FT_Load_Char(face, cp, FT_LOAD_RENDER)) continue;
                FT_Bitmap* g = &face->glyph->bitmap;
                int gx = penX + face->glyph->bitmap_left;
                int gy = baseline - face->glyph->bitmap_top;
                for (unsigned int r = 0; r < g->rows; ++r) {
                    for (unsigned int c = 0; c < g->width; ++c) {
                        int fx = gx + (int)c, fy = gy + (int)r;
                        if (fx < 0 || fx >= tw || fy < 0 || fy >= th) continue;
                        unsigned char ga = g->buffer[r * g->pitch + c];
                        if (!ga) continue;
                        int idx = (fy * tw + fx) * 4;
                        bmp[idx] = 255; bmp[idx+1] = 255; bmp[idx+2] = 255; bmp[idx+3] = ga;
                    }
                }
                penX += (int)(face->glyph->advance.x >> 6);
            }
        }

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp);
        delete[] bmp;

        globalTextCache.put(key, CachedText{ tex, tw, th, 0, 0 });
        outW = (float)tw; outH = (float)th;
        return tex;
    }

    // توليد صورة أيقونة (حرف واحد من خط الأيقونات)
    GLuint glm_volume_icon_texture(const std::string& fontPathIn, int size, uint32_t cp, float& outW, float& outH) {
        if (fontPathIn.empty() || cp == 0 || size <= 0) return 0;
        std::string fontPath = (!fontPathIn.empty() && fontPathIn[0] == '/')
                             ? fontPathIn : (currentScreen.pluginBasePath + fontPathIn);
        std::string key = "glmvol_icn_" + fontPath + "_" + std::to_string(size) + "_" + std::to_string(cp);
        if (const CachedText* c = globalTextCache.get(key)) {
            outW = (float)c->w; outH = (float)c->h;
            return c->texId;
        }

        FT_Face face = get_ft_face(fontPath);
        if (!face) return 0;
        FT_Set_Pixel_Sizes(face, 0, size);
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER)) return 0;

        FT_Bitmap* g = &face->glyph->bitmap;
        int gw = (int)g->width, gh = (int)g->rows;
        if (gw <= 0 || gh <= 0) return 0;

        unsigned char* bmp = new (std::nothrow) unsigned char[(size_t)gw * gh * 4]();
        if (!bmp) return 0;
        for (int r = 0; r < gh; ++r) {
            for (int c = 0; c < gw; ++c) {
                int idx = (r * gw + c) * 4;
                bmp[idx] = 255; bmp[idx+1] = 255; bmp[idx+2] = 255;
                bmp[idx+3] = g->buffer[r * g->pitch + c];
            }
        }

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gw, gh, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp);
        delete[] bmp;

        globalTextCache.put(key, CachedText{ tex, gw, gh, 0, 0 });
        outW = (float)gw; outH = (float)gh;
        return tex;
    }

    // 🚀 رسم شريط الصوت فوق كل الطبقات (يُستدعى قبل eglSwapBuffers مباشرة)
    void draw_volume_overlay(int screenW, int screenH, float currentTime) {
        if (!volume_active.load() || program == 0) return;

        // 1) التوقيت: فيد-إن ثم بقاء ثم فيد-آوت تلقائي
        if (volume_show_start.load() < 0.0f)  volume_show_start.store(currentTime);
        if (volume_touch_time.load() < 0.0f)  volume_touch_time.store(currentTime);

        float sinceShow  = currentTime - volume_show_start.load();
        float sinceTouch = currentTime - volume_touch_time.load();
        if (sinceShow  < 0.0f) { volume_show_start.store(currentTime);  sinceShow  = 0.0f; }
        if (sinceTouch < 0.0f) { volume_touch_time.store(currentTime); sinceTouch = 0.0f; }

        const float FADE_IN  = 0.18f;
        const float FADE_OUT = 0.35f;
        float timeout = volume_timeout.load();

        float alpha = (FADE_IN > 0.0f) ? fmin(1.0f, sinceShow / FADE_IN) : 1.0f;
        if (sinceTouch > timeout) {
            float p = (sinceTouch - timeout) / FADE_OUT;
            if (p >= 1.0f) {
                // انتهى العرض: نطفئ كل شيء ونسمح للكاش الثابت بالعودة
                volume_active.store(false);
                volume_show_start.store(-1.0f);
                volume_touch_time.store(-1.0f);
                volume_last_draw_time = -1.0f;
                invalidate_static_scene_cache();
                invalidate_layer_scene_cache();
                force_render = true;
                return;
            }
            alpha = fmin(alpha, 1.0f - p);
        }
        // تنعيم الظهور/الاختفاء
        alpha = alpha * alpha * (3.0f - 2.0f * alpha);
        if (alpha <= 0.001f) { force_render = true; return; }

        // 2) نسخة آمنة من الإعدادات
        VolumeBarStyle st;
        { std::lock_guard<std::mutex> vlock(volumeStyleMutex); st = volumeStyle; }

        int   vol   = volume_value.load();
        bool  muted = volume_muted.load();
        if (vol < 0)   vol = 0;
        if (vol > 100) vol = 100;

        // 3) تحريك ناعم لمستوى التعبئة
        float target = vol / 100.0f;
        float dt = (volume_last_draw_time < 0.0f) ? 0.016f : (currentTime - volume_last_draw_time);
        volume_last_draw_time = currentTime;
        if (dt < 0.0f)   dt = 0.016f;
        if (dt > 0.100f) dt = 0.100f;
        if (volume_fill_snap.exchange(false)) volume_display_fill = target;
        else {
            float k = dt * 16.0f; if (k > 1.0f) k = 1.0f;
            volume_display_fill += (target - volume_display_fill) * k;
        }
        if (fabs(target - volume_display_fill) < 0.0015f) volume_display_fill = target;


        // 4) الهندسة
        float panelW = st.w, panelH = st.h;
        float panelX = st.centerX ? ((screenW - panelW) / 2.0f) : st.x;
        float panelY = st.y;

        glm::mat4 proj = glm::ortho(0.0f, (float)screenW, (float)screenH, 0.0f);
        // حركة ظهور أنيقة: تكبير خفيف + صعود بسيط
        float appear = fmin(1.0f, sinceShow / FADE_IN);
        appear = appear * appear * (3.0f - 2.0f * appear);
        float scale  = 0.96f + 0.04f * appear;
        float riseY  = (1.0f - appear) * 14.0f;
        {
            float cx = panelX + panelW / 2.0f;
            float cy = panelY + panelH / 2.0f;
            proj = glm::translate(proj, glm::vec3(cx, cy + riseY, 0.0f));
            proj = glm::scale(proj, glm::vec3(scale, scale, 1.0f));
            proj = glm::translate(proj, glm::vec3(-cx, -cy, 0.0f));
        }

        // 5) تجهيز حالة الرسم
        glDisable(GL_SCISSOR_TEST);
        gl_set_blend(true);
        gl_use_program(program);
        glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(proj));
        glEnableVertexAttribArray(posLoc);
        glEnableVertexAttribArray(texCoordLoc);
        float vTexCoords[] = { 0,0, 1,0, 0,1, 1,1 };
        glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, vTexCoords);

        auto drawRect = [&](float rx, float ry, float rw, float rh, float rad,
                            float cr, float cg, float cb, float ca) {
            if (rw <= 0.0f || rh <= 0.0f || ca <= 0.0f) return;
            float maxRad = fmin(rw, rh) / 2.0f;
            if (rad > maxRad) rad = maxRad;
            if (rad < 0.0f)   rad = 0.0f;
            glUniform1f(useTexLoc, 0.0f);
            glUniform1f(gradientModeLoc, 0.0f);
            glUniform1f(borderWidthLoc, 0.0f);
            glUniform1f(roundLoc, rad);
            glUniform2f(rectPosLoc, rx, ry);
            glUniform2f(boxSizeLoc, rw, rh);
            glUniform4f(colorLoc, cr, cg, cb, ca);
            float v[] = { rx, ry, rx + rw, ry, rx, ry + rh, rx + rw, ry + rh };
            glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, v);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        };

        auto drawTex = [&](GLuint tex, float rx, float ry, float rw, float rh,
                           float cr, float cg, float cb, float ca) {
            if (tex == 0 || rw <= 0.0f || rh <= 0.0f || ca <= 0.0f) return;
            glUniform1f(roundLoc, 0.0f);
            glUniform1f(borderWidthLoc, 0.0f);
            glUniform1f(gradientModeLoc, 0.0f);
            glUniform1f(useTexLoc, 1.0f);
            glBindTexture(GL_TEXTURE_2D, tex);
            glUniform4f(colorLoc, cr, cg, cb, ca);
            float v[] = { rx, ry, rx + rw, ry, rx, ry + rh, rx + rw, ry + rh };
            glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, v);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        };

        // 6) اللوحة الخلفية
        drawRect(panelX, panelY, panelW, panelH, st.radius,
                 st.panelR, st.panelG, st.panelB, st.panelA * alpha);

        float padX = panelH * 0.30f;
        float cursorX = panelX + padX;

        // 7) الأيقونة
        float iconW = 0.0f, iconH = 0.0f;
        uint32_t cp = muted ? st.iconMutedCp : st.iconCp;
        GLuint iconTex = glm_volume_icon_texture(st.iconFontPath, st.iconFontSize, cp, iconW, iconH);
        if (iconTex != 0 && iconW > 0.0f && iconH > 0.0f) {
            float iy = panelY + (panelH - iconH) / 2.0f;
            float ir = muted ? st.muteR : st.iconR;
            float ig = muted ? st.muteG : st.iconG;
            float ib = muted ? st.muteB : st.iconB;
            drawTex(iconTex, cursorX, iy, iconW, iconH, ir, ig, ib, alpha);
            cursorX += iconW + padX * 0.75f;
        }

        // 8) نص النسبة (يُرسم على اليمين)
        float textW = 0.0f, textH = 0.0f;
        GLuint textTex = 0;
        float rightLimit = panelX + panelW - padX;
        if (st.showPercent) {
            std::string label = std::to_string(vol) + "%";
            textTex = glm_volume_text_texture(st.fontPath, st.fontSize, label, textW, textH);
            if (textTex != 0) {
                // نحجز عرضاً ثابتاً حتى لا يرتجف الشريط عند تغير عدد الخانات
                float reserved = fmax(textW, panelH * 0.95f);
                float tx = panelX + panelW - padX - textW;
                float ty = panelY + (panelH - textH) / 2.0f;
                drawTex(textTex, tx, ty, textW, textH,
                        muted ? st.muteR : st.textR,
                        muted ? st.muteG : st.textG,
                        muted ? st.muteB : st.textB,
                        (muted ? st.muteA : st.textA) * alpha);
                rightLimit = panelX + panelW - padX - reserved - padX * 0.5f;
            }
        }

        // 9) الشريط (المسار + التعبئة)
        float trackX = cursorX;
        float trackW = rightLimit - trackX;
        if (trackW > 8.0f) {
            float trackH = (st.barHeight > 0.0f) ? st.barHeight : fmax(6.0f, panelH * 0.11f);
            float trackY = panelY + (panelH - trackH) / 2.0f;
            float rad = st.barRadius;

            drawRect(trackX, trackY, trackW, trackH, rad,
                     st.trackR, st.trackG, st.trackB, st.trackA * alpha);

            float f = volume_display_fill;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            float fillW = trackW * f;
            // لا نرسم قطعة أصغر من الانحناء (تظهر مشوهة)
            if (fillW > 1.0f) {
                if (fillW < trackH) fillW = trackH;
                drawRect(trackX, trackY, fillW, trackH, rad,
                         muted ? st.muteR : st.fillR,
                         muted ? st.muteG : st.fillG,
                         muted ? st.muteB : st.fillB,
                         (muted ? st.muteA : st.fillA) * alpha);
            }
        }

        // طالما الشريط ظاهر يجب أن يستمر المحرك بالرسم (للفيد والإخفاء التلقائي)
        force_render = true;
    }

    // 🚀 مسار خاص: رسم الشريط وحده فوق الفيديو عندما تكون الواجهة مخفية
    void internal_render_volume_only(int w, int h, float currentTime) {
        if ((!using_vugles && display == EGL_NO_DISPLAY) || program == 0) return;
        std::lock_guard<std::mutex> lock(screenMutex);

        glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo); gl_state_cache_reset();
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // شفاف تماماً ليبقى الفيديو ظاهراً
        glClear(GL_COLOR_BUFFER_BIT);

        draw_volume_overlay(w, h, currentTime);
        glm_present();
    }

    // ❌ حُذفت can_use_static_scene_cache(): كانت ميتة تماماً لأن staticCacheAllowed
    //    مثبّت على false في internal_render_frame. أُبقيت بقية منظومة الـ static FBO
    //    كما هي (بلا تغيير في سلوك الرسم) تحسباً لإعادة تفعيلها لاحقاً.

    void draw_static_scene_cache_to_screen(int w, int h) {
        if (!staticSceneValid || staticSceneTex == 0 || fastProgram == 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo); gl_state_cache_reset();
        glDisable(GL_SCISSOR_TEST);
        gl_set_blend(false);
        glViewport(0, 0, w, h);
        {
            float hardwareAlpha = (!backdrop_hidden.load()) ? 1.0f : currentScreen.bgA;
            glClearColor(currentScreen.bgR, currentScreen.bgG, currentScreen.bgB, hardwareAlpha);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        // 🚀 السحر هنا: نستخدم مصفوفة محايدة (Identity Matrix) لمنع تضاعف الخطأ
        glm::mat4 identity(1.0f);
        gl_use_program(fastProgram);
        glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(identity));
        glUniform1f(fastUseTexLoc, 1.0f);
        glUniform4f(fastColorLoc, 1.0f, 1.0f, 1.0f, 1.0f);
        glBindTexture(GL_TEXTURE_2D, staticSceneTex);

        // 🚀 تمرير إحداثيات الشاشة المطلقة (NDC) مباشرة لتخطي حسابات كرت الشاشة الضعيفة
        float v[] = { -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f };
        float tc[] = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

        glEnableVertexAttribArray(fastPosLoc);
        glEnableVertexAttribArray(fastTexCoordLoc);
        glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, v);
        glVertexAttribPointer(fastTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, tc);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    bool ensure_layer_scene_fbo(int w, int h, int splitZ) {
        if (w <= 0 || h <= 0) return false;

        if (layerSceneFbo != 0 && layerSceneTex != 0 && layerSceneW == w && layerSceneH == h && layerSceneSplitZ == splitZ) {
            return true;
        }

        if (layerSceneTex != 0) { glDeleteTextures(1, &layerSceneTex); layerSceneTex = 0; }
        if (layerSceneFbo != 0) { glDeleteFramebuffers(1, &layerSceneFbo); layerSceneFbo = 0; }

        glGenTextures(1, &layerSceneTex);
        glBindTexture(GL_TEXTURE_2D, layerSceneTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glGenFramebuffers(1, &layerSceneFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, layerSceneFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layerSceneTex, 0);
        bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo);

        if (!ok) {
            if (layerSceneTex != 0) { glDeleteTextures(1, &layerSceneTex); layerSceneTex = 0; }
            if (layerSceneFbo != 0) { glDeleteFramebuffers(1, &layerSceneFbo); layerSceneFbo = 0; }
            layerSceneW = 0; layerSceneH = 0; layerSceneSplitZ = 999999; layerSceneValid = false; layerSceneDirty.store(true);
            return false;
        }

        layerSceneW = w;
        layerSceneH = h;
        layerSceneSplitZ = splitZ;
        layerSceneValid = false;
        layerSceneDirty.store(true);
        return true;
    }

    // ==================================================================================
    // 🪟 أدوات لقطات الطبقات المتراكبة
    // ==================================================================================
    bool glm_create_snapshot_target(int w, int h, GLuint& texOut, GLuint& fboOut) {
        texOut = 0; fboOut = 0;
        if (w <= 0 || h <= 0) return false;

        glGenTextures(1, &texOut);
        glBindTexture(GL_TEXTURE_2D, texOut);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glGenFramebuffers(1, &fboOut);
        glBindFramebuffer(GL_FRAMEBUFFER, fboOut);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texOut, 0);
        bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo);
        gl_state_cache_reset();

        if (!ok) {
            glDeleteTextures(1, &texOut);
            glDeleteFramebuffers(1, &fboOut);
            texOut = 0; fboOut = 0;
            GLM_LOG("[GLM-STACK] FBO incomplete: snapshot target creation failed.\n");
            return false;
        }
        return true;
    }

    void glm_destroy_snapshot_target(GLuint& tex, GLuint& fbo) {
        if (tex != 0) { glDeleteTextures(1, &tex); tex = 0; }
        if (fbo != 0) { glDeleteFramebuffers(1, &fbo); fbo = 0; }
    }

    // 🪟 رسم لقطة مجمّدة ملء الهدف الحالي.
    //    dim   : 0 = بلا تعتيم، 1 = أسود تماماً.
    //    alpha : 1 = نسخة مصمتة (بلا مزج)، أقل = تلاشي فوق ما تحته.
    void glm_draw_snapshot_fullscreen(GLuint tex, float dim, float alpha) {
        if (tex == 0 || snapProgram == 0) return;

        float k = 1.0f - (dim < 0.0f ? 0.0f : (dim > 1.0f ? 1.0f : dim));
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        gl_set_blend(alpha < 0.999f);
        glm::mat4 identity(1.0f);
        gl_use_program(snapProgram);
        glUniformMatrix4fv(snapProjLoc, 1, GL_FALSE, glm::value_ptr(identity));
        glUniform1f(snapDimLoc, k);
        glUniform1f(snapAlphaLoc, alpha);
        glBindTexture(GL_TEXTURE_2D, tex);

        // 🚀 إحداثيات NDC مطلقة: نفس اتجاه draw_layer_scene_cache_to_screen تماماً،
        //    فتتطابق اللقطة سواء رُسمت على الشاشة أو داخل FBO آخر.
        float v[]  = { -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f };
        float tc[] = {  0.0f, 1.0f, 1.0f, 1.0f,  0.0f,  0.0f, 1.0f,  0.0f };
        glEnableVertexAttribArray(snapPosLoc);
        glEnableVertexAttribArray(snapTexCoordLoc);
        glVertexAttribPointer(snapPosLoc, 2, GL_FLOAT, GL_FALSE, 0, v);
        glVertexAttribPointer(snapTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, tc);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // void draw_layer_scene_cache_to_screen(int w, int h) {
    //     if (!layerSceneValid || layerSceneTex == 0 || fastProgram == 0) return;

    //     glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo);
    //     glDisable(GL_SCISSOR_TEST);
    //     gl_set_blend(false);
    //     glViewport(0, 0, w, h);
    //     // 🚀 منع رمشة/بقايا البفر عند عرض لقطة FBO: ننظف الـ default framebuffer بنفس Alpha الصحيح.
    //     {
    //         float hardwareAlpha = (!backdrop_hidden.load()) ? 1.0f : currentScreen.bgA;
    //         glClearColor(currentScreen.bgR, currentScreen.bgG, currentScreen.bgB, hardwareAlpha);
    //         glClear(GL_COLOR_BUFFER_BIT);
    //     }

    //     glm::mat4 proj = glm::ortho(0.0f, (float)w, (float)h, 0.0f);
    //     gl_use_program(fastProgram);
    //     glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(proj));
    //     glUniform1f(fastUseTexLoc, 1.0f);
    //     glUniform4f(fastColorLoc, 1.0f, 1.0f, 1.0f, 1.0f);
    //     glBindTexture(GL_TEXTURE_2D, layerSceneTex);

    //     float v[] = { 0.0f, 0.0f, (float)w, 0.0f, 0.0f, (float)h, (float)w, (float)h };
    //     float tc[] = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    //     glEnableVertexAttribArray(fastPosLoc);
    //     glEnableVertexAttribArray(fastTexCoordLoc);
    //     glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, v);
    //     glVertexAttribPointer(fastTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, tc);
    //     glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // }
    void draw_layer_scene_cache_to_screen(int w, int h) {
        if (!layerSceneValid || layerSceneTex == 0 || fastProgram == 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo); gl_state_cache_reset();
        glDisable(GL_SCISSOR_TEST);
        gl_set_blend(false);
        glViewport(0, 0, w, h);
        {
            float hardwareAlpha = (!backdrop_hidden.load()) ? 1.0f : currentScreen.bgA;
            glClearColor(currentScreen.bgR, currentScreen.bgG, currentScreen.bgB, hardwareAlpha);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        // 🚀 مصفوفة محايدة لمنع اهتزاز الطبقات
        glm::mat4 identity(1.0f);
        gl_use_program(fastProgram);
        glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(identity));
        glUniform1f(fastUseTexLoc, 1.0f);
        glUniform4f(fastColorLoc, 1.0f, 1.0f, 1.0f, 1.0f);
        glBindTexture(GL_TEXTURE_2D, layerSceneTex);

        // 🚀 إحداثيات مطلقة ثابتة كالصخر
        float v[] = { -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f };
        float tc[] = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        
        glEnableVertexAttribArray(fastPosLoc);
        glEnableVertexAttribArray(fastTexCoordLoc);
        glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, v);
        glVertexAttribPointer(fastTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, tc);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // 🔒 تُستدعى من مسار الرسم فقط، و screenMutex مقفول أصلاً من internal_render_frame.
    void process_ready_textures() {
        // 1. فحص حالة حركة القوائم — **قبل** أخذ queueMutex.
        //    أثناء الكاروسال نخرج فوراً، فلا يلمس مسار الرسم هذا القفل إطلاقاً
        //    ولا يمكن لأي عامل أن يعطّل إطاراً مهما طال احتجازه للقفل.
        //    (قراءة currentScreen آمنة هنا لأن screenMutex محجوز من المستدعي.)
        bool isMoving = false;
        for (size_t i = 0; i < currentScreen.widgets.size(); ++i) {
            if (currentScreen.widgets[i].isAnimating) { isMoving = true; break; }
        }

        // 🚀 أثناء حركة الكاروسال أو أنيميشن دخول/خروج Grid لا نرفع أي Texture للـ GPU، حتى الباكدروب.
        // glTexImage2D في منتصف الحركة هو أكثر سبب للتقطيع المتقطع.
        if (isMoving || currentScreen.slideActive) {
            // ⚠️ لكن إن وُجدت بيانات جاهزة تنتظر الرفع، يجب أن نضمن إطاراً آخر
            // فور انتهاء الحركة. بدون ذلك: الإطار الأخير من الأنيميشن يتخطى الرفع،
            // ثم تُطفأ أعلام الحركة فلا يبقى سبب لرسم إطار جديد، وتظل الصورة
            // معلّقة حتى يصل أمر من بايثون (نبضة الساعة مثلاً) — أي تأخير يصل
            // إلى ثانية كاملة قبل ظهور الصورة.
            if (bgTaskData.load() != nullptr) {
                force_render = true;
            } else {
                for (int i = 0; i < MAX_TEXTURE_POOL; i++) {
                    if (texturePool[i].dataReady.load()) { force_render = true; break; }
                }
            }
            return;
        }

        std::lock_guard<std::mutex> lock(queueMutex);

        // 2. تحديث الخلفية (باكدروب) بعد توقف الحركة فقط
        unsigned char* bData = bgTaskData.exchange(nullptr);
        if (bData != nullptr) {
            bool matches = (bgTaskReadyPath == bgTaskPath);
            if (!matches) {
                // 🚀 البيانات تخص بوستر سابق (المستخدم تحرك قبل الرفع)، نتخلص منها فوراً
                stbi_image_free(bData);
            } else {
                if (currentScreen.currentBg.textureId != 0) {
                    if (currentScreen.oldBg.textureId != 0) glDeleteTextures(1, &currentScreen.oldBg.textureId);
                    currentScreen.oldBg = currentScreen.currentBg;
                }
                
                currentScreen.currentBg.imagePath = bgTaskPath;
                currentScreen.currentBg.hasAlpha = false;
                currentScreen.bgBakedMask = bgTaskReadyMask;  // 🎭 توثيق القناع المخبوز
            
            glGenTextures(1, &currentScreen.currentBg.textureId);
            glBindTexture(GL_TEXTURE_2D, currentScreen.currentBg.textureId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            int bw = bgTaskW.load();
            int bh = bgTaskH.load();
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, bData);
            
            stbi_image_free(bData);
            currentScreen.currentBg.loaded = true;
            currentScreen.isBgFading = true;
            currentScreen.bgFadeStartTime = -1.0f;
            staticSceneDirty.store(true);
            staticSceneValid = false;
            invalidate_layer_scene_cache();
            }
        }

        // 3. تحديث البوسترات وضخها إلى كرت الشاشة
        int uploadsThisFrame = 0; 
        
        // إذا كان أنيميشن الصعود الشامل نشطاً، نرفع المقدار إلى 50 بوستر ليتم ضخ الصفحة بالكامل دفعة واحدة
        //int maxUploads = currentScreen.slideActive ? 50 : 2;
        // ✅ التعديل: رفع الحد الأقصى من 2 إلى 15 لتظهر جميع البوسترات المرئية في الكاش دفعة واحدة
        //int maxUploads = currentScreen.slideActive ? 50 : 15;
        int maxUploads = currentScreen.slideActive ? 1 : 10;

        for (int i = 0; i < MAX_TEXTURE_POOL; i++) {
            if (texturePool[i].dataReady) {
                if (texturePool[i].inUse && texturePool[i].pixelData) {
                    glBindTexture(GL_TEXTURE_2D, texturePool[i].textureId);
                    int imgW = texturePool[i].tempW.load();
                    int imgH = texturePool[i].tempH.load();
                    if (texturePool[i].width == imgW && texturePool[i].height == imgH) {
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, imgW, imgH, GL_RGBA, GL_UNSIGNED_BYTE, texturePool[i].pixelData);
                    } else {
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, imgW, imgH, 0, GL_RGBA, GL_UNSIGNED_BYTE, texturePool[i].pixelData);
                        texturePool[i].width = imgW;
                        texturePool[i].height = imgH;
                    }
                } else if (texturePool[i].inUse && !texturePool[i].pixelData) {
                    // 🚀 الحماية: تصفير الأبعاد إذا فشل التحميل (رابط ميت) لمنع ظهور الصورة القديمة كـ "رمشة"
                    texturePool[i].width = 0;
                    texturePool[i].height = 0;
                }
                if (texturePool[i].pixelData) {
                    stbi_image_free(texturePool[i].pixelData);
                    texturePool[i].pixelData = nullptr;
                }
                texturePool[i].dataReady = false;
                texturePool[i].isLoading = false;
                staticSceneDirty.store(true);
                staticSceneValid = false;
                invalidate_layer_scene_cache();

                uploadsThisFrame++;
                if (uploadsThisFrame >= maxUploads) break; 
            }
        }
    }

    // ==================================================================================
    // 6. الرسم (Renderer)
    // ==================================================================================
    // 🪟 الغلاف: يأخذ القفل ثم ينادي النواة. push/pop ينادون النواة مباشرة لأنهم
    //    يمسكون screenMutex أصلاً (std::mutex غير قابل لإعادة الدخول).
    void internal_render_frame(int w, int h, float currentTime) {
        std::lock_guard<std::mutex> lock(screenMutex);
        internal_render_frame_locked(w, h, currentTime);
    }

    void internal_render_frame_locked(int w, int h, float currentTime) {
        if (program == 0) return;
        static int frame_counter = 0;
        if (frame_counter == 0) {
            GLM_LOG("[GLM-DEBUG] ==========================================\n");
            GLM_LOG("[GLM-DEBUG] 3. FIRST RENDER FRAME EXECUTED!\n");
            GLM_LOG("[GLM-DEBUG] ==========================================\n");
        }
        frame_counter++;
        engine_last_frame_time.store(currentTime); // 🚀 لإعادة رسم نفس المشهد عند الخروج
        // ✅ الحماية الأهم: القفل يُؤخذ في الغلاف internal_render_frame أعلاه

        // ✅ التحقق إذا كان هناك أي صور جديدة جاهزة لرفعها لكرت الشاشة
        process_ready_textures();

        // ==============================================================================
        // 🏷️ تحديث المعرّفات الرقمية مرة واحدة لكل إطار
        // ==============================================================================
        // بدل مقارنة std::string عشرات المرات لكل عنصر داخل حلقة الرسم،
        // نحوّل السلاسل إلى أرقام هنا فقط. المصدر يبقى السلسلة دائماً،
        // لذا لا يمكن أن تتباعد القيمتان مهما تغيّرت من XML أو من بايثون.
        // ==============================================================================
        currentScreen.animTypeId = anim_id_of(currentScreen.animType);
        for (auto& e : currentScreen.images)    e.animId = anim_id_of(e.anim);
        for (auto& e : currentScreen.gradients) e.animId = anim_id_of(e.anim);
        for (auto& e : currentScreen.labels) {
            e.animId  = anim_id_of(e.anim);
            e.alignId = align_id_of(e.textAlign);
        }
        for (auto& e : currentScreen.widgets) {
            e.animId    = anim_id_of(e.anim);
            e.orientId  = orient_id_of(e.orientation);
            e.itemShape = shape_id_of(e.itemType);
        }
        const AnimId screenAnimId = currentScreen.animTypeId;

        // تعطيل Static FBO أثناء التنقل لمنع اختلاف موضع الـ Avatar و itemcolor
        bool staticCacheAllowed = false;
        bool staticCacheReady = staticCacheAllowed && staticSceneValid && !staticSceneDirty.load() &&
                                staticSceneTex != 0 && staticSceneW == w && staticSceneH == h;

        // 🚀 إذا كان كل شيء ثابتاً ولدينا لقطة جاهزة، نرسم Texture واحدة فقط ونخرج.
        // هذا هو تجميد الخلفية/البوسترات عملياً لتخفيف الحمل على الـ GPU.
        if (staticCacheReady) {
            draw_static_scene_cache_to_screen(w, h);
            draw_volume_overlay(w, h, currentTime); // 🔊 حماية: الشريط يظهر حتى فوق اللقطة المجمدة
            glm_present();
            return;
        }

        // 🚀 زمن فيزياء الحركة المستمرة. نستخدمه للـ Prime-style smoothing بدل t ثابت لكل ضغطة.
        static float lastPhysicsTime = -1.0f;
        // ⏱️ بعد أي فترة سكون تكون الفجوة كبيرة (الساعة تتقدّم بلا رسم)، والقصّ
        //    عند 33ms يظل ضعف الإطار الطبيعي ⇒ أول إطار حركة يقطع ~48% من
        //    المسافة بدل ~27% فيبدو كأن البوستر ارتدّ ثم تقدّم. نبدأ بزمن اسمي.
        if (physics_resync.exchange(false)) lastPhysicsTime = -1.0f;
        float frameDt = (lastPhysicsTime < 0.0f) ? 0.016f : (currentTime - lastPhysicsTime);
        lastPhysicsTime = currentTime;
        if (frameDt < 0.001f) frameDt = 0.001f;
        if (frameDt > 0.033f) frameDt = 0.033f;

        // 🚀 Layered FBO أثناء حركة Widget فوق الخلفية/البوسترات:
        // نجمّد كل zPosition الأقل من الويدجت المتحرك، ثم نرسم الويدجت فقط فوق اللقطة.
        int dynamicMinZ = 999999;
        bool hasDynamicWidget = false;   // للتوثيق: يبيّن هل يوجد عنصر متحرك في هذا الإطار
        (void)hasDynamicWidget;

        for (const auto& img : currentScreen.images) {
            bool dyn = currentScreen.slideActive && !img.anim.empty() && img.anim != "none";
            if (dyn) { hasDynamicWidget = true; dynamicMinZ = std::min(dynamicMinZ, img.z); }
        }
        for (const auto& lbl : currentScreen.labels) {
            bool dyn = (currentScreen.slideActive && !lbl.anim.empty() && lbl.anim != "none") ||
                       (lbl.scrollTextMode != 0 && lbl.textScrollActive);
            if (dyn) { hasDynamicWidget = true; dynamicMinZ = std::min(dynamicMinZ, lbl.z); }
        }
        for (const auto& wdg : currentScreen.widgets) {
            // 📏 شريط التمرير أثناء ظهوره/تلاشيه يجعل الويدجت حيّاً، وإلا تجمّد
            //    طبقته في Layer FBO ووقف التلاشي في منتصفه. عند وصول الشفافية إلى
            //    صفر يعود التجميد تلقائياً (شرط ذاتي الإنهاء).
            bool scrollbarBusy = (wdg.scrollbarMode != 0 && wdg.sliderAnim &&
                                  (wdg.scrollbarAlpha > 0.001f || wdg.scrollbarLastMoveTime < 0.0f));

            // 🔍 انتقال التكبير يحتاج إطارات حيّة وإلا تجمّدت الطبقة فلم يتقدّم.
            //    ذاتي الإنهاء: ينتهي فور بلوغ الهدف فيعود التجميد.
            bool zoomShifting = (wdg.zoomShiftStart >= 0.0f) || (wdg.zoomShift != wdg.zoomShiftTo);

            bool dyn = wdg.isAnimating || wdg.localAnimActive || (wdg.scrollTextMode != 0 && wdg.textScrollActive) ||
                       scrollbarBusy || zoomShifting ||
                       (currentScreen.slideActive && !wdg.anim.empty() && wdg.anim != "none");
            if (dyn) { hasDynamicWidget = true; dynamicMinZ = std::min(dynamicMinZ, wdg.z); }
        }

        // لا نمنع Layered FBO بسبب تحميل بوسترات في الخلفية؛ أي صور تصل أثناء الحركة تبقى مؤجلة
        // أصلاً بواسطة process_ready_textures، والأهم هنا نعومة الحركة.
        // نمنعه فقط لو كان هناك أنيميشن شامل للشاشة كلها لأن الطبقات السفلى نفسها تتحرك.
        // 🚀 لا نستخدم Layered FBO أثناء وضع التريلر أو أثناء fade الباكدروب.
        // السبب: قد يحتوي الـ layer cache على باكدروب قديم فيُرسم فوق الفيديو وتختفي mask_trailer.
        // 🚀 السحر هنا: السماح بـ Layered FBO أثناء حركة الكاروسال "فقط" إذا كان غطاء التريلر مفعلًا (لأنه ثابت ومعتم).
        // هذا يضمن نعومة الانزلاق التامة دون إفساد التلاشي (Fade) لباقي العناصر.
        // لا نستخدم Layer FBO أثناء إعادة رسم القوائم المتحركة
        // 🚀 السحر هنا: تفعيل Layer FBO بشكل افتراضي لتجميد المشاهد الثقيلة
        bool layeredCacheAllowed = true;

        // 🪟 أثناء التقاط لقطة طبقة نريد مشهداً كاملاً في FBO واحد بلا أي كاش وسيط.
        if (g_capturing_snapshot) layeredCacheAllowed = false;

        if (!trailer_cover_visible.load()) {
            // 🚀 الحماية: إيقاف التجميد فوراً أثناء تلاشي الباكدروب (Fade) لكي لا يتجمد وهو مخفي (شفاف)!
            if (backdrop_hidden.load() || trailer_fade_start.load() >= 0.0f || 
                trailer_cover_alpha.load() > 0.0f || currentScreen.isBgFading || 
                currentScreen.isMaskFading || currentScreen.screenFadeState != 0 ||
                currentScreen.slideActive) { // 👈 أضف هذا الشرط هنا لمنع تجميد الأنيميشن
                layeredCacheAllowed = false;
            }
        }

        // ==============================================================================
        // 🎨 Adaptive Background Color — تحديث الألوان مرة واحدة لكل إطار (خارج حلقة الرسم)
        // ==============================================================================
        // لماذا هنا تحديداً: القناع التكيّفي يوضع في طبقة أدنى من القائمة المتحركة، فيتجمّد
        // داخل Layer FBO ولا يُرسم إطلاقاً أثناء حركة الكاروسال (تكلفة صفر). لكن ذلك يعني
        // أن كود الرسم لا يُنفَّذ، فلا مكان آخر لتقدّم الانتقال اللوني. لذلك نحدّثه هنا،
        // ونُبطل الكاش فقط طوال مدة الانتقال (وهي تبدأ بعد أن تستقر القائمة، لا أثناءها).
        {
            bool listMoving = false;
            for (const auto& wd : currentScreen.widgets) {
                if (wd.isAnimating) { listMoving = true; break; }
            }
            bool adaptiveBlending = false;

            for (auto& aimg : currentScreen.images) {
                if (!aimg.adaptiveTint) continue;

                float tR = aimg.adpFallR, tG = aimg.adpFallG, tB = aimg.adpFallB;
                if (!aimg.adaptiveSource.empty()) {
                    for (const auto& srcW : currentScreen.widgets) {
                        if (srcW.name != aimg.adaptiveSource) continue;
                        const int si = srcW.selectedIndex;
                        if (si >= 0 && si < (int)srcW.items.size() && srcW.items[si].domValid) {
                            tR = srcW.items[si].domR; tG = srcW.items[si].domG; tB = srcW.items[si].domB;
                        }
                        break;
                    }
                }
                tR *= aimg.adaptiveDim; tG *= aimg.adaptiveDim; tB *= aimg.adaptiveDim;

                if (!aimg.adpInit) {
                    aimg.adpCurR = aimg.adpFromR = aimg.adpTgtR = tR;
                    aimg.adpCurG = aimg.adpFromG = aimg.adpTgtG = tG;
                    aimg.adpCurB = aimg.adpFromB = aimg.adpTgtB = tB;
                    aimg.adpInit = true;
                    continue;
                }

                const bool changed = (fabs(tR - aimg.adpTgtR) > 0.002f ||
                                      fabs(tG - aimg.adpTgtG) > 0.002f ||
                                      fabs(tB - aimg.adpTgtB) > 0.002f);

                // 🚀 لا ننطلق في انتقال لوني والقائمة ما زالت تتحرك (يمنع إعادة بناء
                //    الكاش في كل إطار أثناء التصفّح السريع)، لكن ⚠️ لا نلمس adpTgt أيضاً:
                //    تحديثه هنا كان يجعل الفرق يختفي، فلا يبدأ أي انتقال بعد الاستقرار
                //    ويبقى اللون عالقاً على أول بوستر. نتركه ونطلب إطاراً إضافياً فقط.
                if (changed) {
                    if (!listMoving) {
                        aimg.adpFromR = aimg.adpCurR; aimg.adpFromG = aimg.adpCurG; aimg.adpFromB = aimg.adpCurB;
                        aimg.adpTgtR  = tR;           aimg.adpTgtG  = tG;           aimg.adpTgtB  = tB;
                        aimg.adpBlendStart = currentTime;
                    } else {
                        // نضمن وصول إطار بعد توقف الحركة لكي ينطلق الانتقال
                        force_render = true;
                    }
                }

                if (aimg.adpBlendStart >= 0.0f && !listMoving) {
                    const float dur = (aimg.adaptiveSpeed > 0.01f) ? aimg.adaptiveSpeed : 0.01f;
                    float bt = (currentTime - aimg.adpBlendStart) / dur;
                    if (bt >= 1.0f) {
                        aimg.adpCurR = aimg.adpTgtR; aimg.adpCurG = aimg.adpTgtG; aimg.adpCurB = aimg.adpTgtB;
                        aimg.adpBlendStart = -1.0f;
                        adaptiveBlending = true;   // إطار أخير لتثبيت اللون النهائي داخل الكاش
                    } else {
                        if (bt < 0.0f) bt = 0.0f;
                        const float e = bt * bt * (3.0f - 2.0f * bt);   // smoothstep
                        aimg.adpCurR = aimg.adpFromR + (aimg.adpTgtR - aimg.adpFromR) * e;
                        aimg.adpCurG = aimg.adpFromG + (aimg.adpTgtG - aimg.adpFromG) * e;
                        aimg.adpCurB = aimg.adpFromB + (aimg.adpTgtB - aimg.adpFromB) * e;
                        adaptiveBlending = true;
                    }
                }
            }

            if (adaptiveBlending) {
                invalidate_layer_scene_cache();   // القناع داخل اللقطة المجمدة ⇒ نجبرها على التحديث
                force_render = true;
            }
        }

        bool layerCacheReady = layeredCacheAllowed && layerSceneValid && !layerSceneDirty.load() &&
                               layerSceneTex != 0 && layerSceneW == w && layerSceneH == h && layerSceneSplitZ == dynamicMinZ;
        bool buildLayerCacheThisFrame = layeredCacheAllowed && !layerCacheReady && ensure_layer_scene_fbo(w, h, dynamicMinZ);
        bool layeredCacheActive = layerCacheReady || buildLayerCacheThisFrame;

        bool buildStaticCacheThisFrame = staticCacheAllowed && !layeredCacheActive && ensure_static_scene_fbo(w, h);
        if (buildStaticCacheThisFrame) {
            glBindFramebuffer(GL_FRAMEBUFFER, staticSceneFbo); gl_state_cache_reset();
        } else if (buildLayerCacheThisFrame) {
            glBindFramebuffer(GL_FRAMEBUFFER, layerSceneFbo); gl_state_cache_reset();
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo); gl_state_cache_reset();
        }

        bool drawStaticBaseNow = !(layeredCacheActive && !buildLayerCacheThisFrame);

        // 🚀 1. نقلنا الفحص اللحظي إلى بداية الفريم قبل رسم ومسح الشاشة
        bool is_really_moving = currentScreen.slideActive;
        for (const auto& wdg : currentScreen.widgets) {
            if (wdg.isAnimating) { is_really_moving = true; break; }
        }

        // 🚀 حسابات الفيد (Crossfade) للباكدروب والتريلر
        bool current_bh = backdrop_hidden.load();
        if (last_backdrop_hidden.load() != current_bh) {
            trailer_fade_start.store(currentTime);
            last_backdrop_hidden.store(current_bh);
        }
        
        float trailerAlpha = 1.0f;
        float tf_start = trailer_fade_start.load();
        
        if (tf_start >= 0.0f) {
            // 🚀 إذا كنا نخفي الباكدروب لفتح التريلر أثناء حركة قائمة، نغلقه فوراً لمنع ثقب شفاف.
            // أما عند إرجاع الباكدروب بعد انتهاء التريلر/التنقل، نسمح بالـ fade حتى لو القائمة تتحرك.
            if (is_really_moving && current_bh) {
                trailer_fade_start.store(-1.0f);
                trailerAlpha = 0.0f;
                
                // Smart Wipe فقط عند دخول وضع التريلر أثناء حركة carousel، وليس عند الرجوع من التريلر.
                bool is_carousel_moving = false;
                for (const auto& wdg : currentScreen.widgets) {
                    if (wdg.isAnimating) { is_carousel_moving = true; break; }
                }

                if (is_carousel_moving) {
                    // 🚀 مهم للتريلر:
                    // لا نحذف currentBg عند إخفاء الباكدروب، لأننا نحتاجه يرجع بالـ fade عند نهاية التريلر.
                    // حذف الخلفية هنا كان يسبب ظهور سواد أو اختفاء الباكدروب بعد رجوعه.
                    currentScreen.isBgFading = false;
                    currentScreen.bgFadeStartTime = -1.0f;
                    if (currentScreen.oldBg.textureId != 0) {
                        glDeleteTextures(1, &currentScreen.oldBg.textureId);
                        currentScreen.oldBg.textureId = 0;
                    }
                    currentScreen.oldBg.loaded = false;
                }
                
            } else {
                float elapsed = currentTime - tf_start;
                float t = fmin(1.0f, elapsed / 0.5f);
                float smooth_t = t * t * (3.0f - 2.0f * t);
                trailerAlpha = current_bh ? (1.0f - smooth_t) : smooth_t;
                if (t >= 1.0f) {
                    trailer_fade_start.store(-1.0f);
                    if (!current_bh) {
                        // انتهى رجوع الباكدروب؛ استمر بالرسم قليلاً لتغطية stopService المتأخر من بايثون.
                        trailer_post_render_until.store(currentTime + 1.5f);
                    }
                }
                force_render = true; // إجبار المحرك على الاستمرار
            }
        } else {
            trailerAlpha = current_bh ? 0.0f : 1.0f;
        }
        if (trailer_post_render_until.load() > currentTime) {
            force_render = true;
        }

        // 🚀 حساب غطاء الخروج من التريلر. يظهر فوراً، ويختفي ب fade قصير بعد توقف الفيديو.
        float coverAlpha = trailer_cover_alpha.load();
        if (trailer_cover_visible.load()) {
            coverAlpha = 1.0f;
            trailer_cover_alpha.store(1.0f);
            trailer_cover_fade_start.store(-1.0f);
        } else if (coverAlpha > 0.0f) {
            float cStart = trailer_cover_fade_start.load();
            if (cStart < 0.0f) {
                trailer_cover_fade_start.store(currentTime);
                cStart = currentTime;
            }
            float ct = fmin(1.0f, (currentTime - cStart) / 0.35f);
            float smoothC = ct * ct * (3.0f - 2.0f * ct);
            coverAlpha = 1.0f - smoothC;
            if (ct >= 1.0f) {
                coverAlpha = 0.0f;
                trailer_cover_alpha.store(0.0f);
                trailer_cover_fade_start.store(-1.0f);
            } else {
                trailer_cover_alpha.store(coverAlpha);
                force_render = true;
            }
        }

        // 🚀 2. السحر الحقيقي: استخدام TrailerAlpha لجعل الشاشة معتمة 100% وإخفاء مشغل الفيديو!
        glViewport(0, 0, w, h);
        // 🚀 تعديل: إغلاق الثقب الشفاف فوراً (1.0f) بمجرد انتهاء التريلر لإخفاء الفيديو المتجمد والرمشة
        float hardwareAlpha = (!current_bh) ? 1.0f : currentScreen.bgA;
        // 🪟 عند وجود طبقات تحتنا يجب أن يكون البفر معتماً بما يكفي لتظهر اللقطة،
        //    وإلا فتحنا ثقباً شفافاً على طبقة الفيديو ونحن نعرض واجهة فوق واجهة.
        if (stackSnapTex != 0 && hardwareAlpha < 1.0f) hardwareAlpha = 1.0f;
        glClearColor(currentScreen.bgR, currentScreen.bgG, currentScreen.bgB, hardwareAlpha);
        glClear(GL_COLOR_BUFFER_BIT);

        // ==============================================================================
        // 🪟 الطبقات السفلية المجمّدة: أول ما يُرسم، فتصبح خلفية الطبقة العليا الحيّة.
        // تُرسم هنا تحديداً كي تُلتقط أيضاً داخل الـ Layer/Static FBO عند تفعيلهما.
        // ==============================================================================
        if (stackSnapTex != 0) {
            glm_draw_snapshot_fullscreen(stackSnapTex, stackDim, 1.0f);
        }

        // 🪟 طبقة جديدة لم تُسلَّح حركتها بعد: نعرض ما تحتها فقط (نفس الإطار السابق تماماً)
        //    بدل رسمها في مواضعها الخام. هذا ما يمنع رمشة غطاء التعتيم عند الفتح.
        if (g_push_hold && !g_capturing_snapshot) {
            if (g_push_hold_start < 0.0f) g_push_hold_start = currentTime;
            if (currentTime - g_push_hold_start < GLM_PUSH_HOLD_MAX) {
                draw_volume_overlay(w, h, currentTime);
                glm_present();
                force_render = true;   // نُبقي الإطارات حيّة حتى يصل أمر الأنيميشن
                return;
            }
            g_push_hold = false;       // انتهت المهلة: نتابع الرسم الطبيعي
        }

        // 🚀 إنشاء مصفوفتين: واحدة ثابتة، والأخرى متحركة للأنيميشن الشامل
        int screenW = w;
        int screenH = h;
        glm::mat4 baseProjection = glm::ortho(0.0f, (float)screenW, (float)screenH, 0.0f);
        glm::mat4 animProjection = baseProjection;

        float screenScale = 1.0f;
        float screenAlpha = 1.0f;
        if (currentScreen.slideActive) {
            float elapsed = 0.0f;
            if (currentScreen.slideStartTime >= 0.0f) {
                elapsed = currentTime - currentScreen.slideStartTime;
            }
            if (screenAnimId == AnimId::ZoomFade) {
                float rawT = fmin(1.0f, elapsed / currentScreen.animDuration);
                float animT = glm_ease_out(currentScreen.easeId, rawT); // 🎚️ Easing من الـ XML
                
                screenScale = 1.25f - (0.25f * animT);
                screenAlpha = animT;
            } else if (currentScreen.animType == "zoom_in_fade_out") { // 🚀 أنيميشن الخروج
                float rawT = fmin(1.0f, elapsed / currentScreen.animDuration);
                float animT = rawT * rawT; // Ease-in (حركة تسارع ناعمة)
                
                screenScale = 1.0f + (0.35f * animT); // يكبر من 100% إلى 135%
                screenAlpha = 1.0f - animT;           // يتلاشى من كامل الوضوح للصفر
            }
        }

        if (screenScale != 1.0f) {
            animProjection = glm::translate(animProjection, glm::vec3((float)screenW / 2.0f, (float)screenH / 2.0f, 0.0f));
            animProjection = glm::scale(animProjection, glm::vec3(screenScale, screenScale, 1.0f));
            animProjection = glm::translate(animProjection, glm::vec3(-(float)screenW / 2.0f, -(float)screenH / 2.0f, 0.0f));
        }

        gl_use_program(program);
        glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(baseProjection)); // تفعيل الثابتة كوضع افتراضي
        glEnableVertexAttribArray(posLoc); glEnableVertexAttribArray(texCoordLoc);
        float texCoords[] = { 0,0, 1,0, 0,1, 1,1 }; glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);

        float elapsedAnimTime = 0.0f;
        if (currentScreen.slideActive) {
            static float animWaitStart = 0.0f;
            if (currentScreen.slideStartTime == -2.0f) {
                animWaitStart = currentTime;
                currentScreen.slideStartTime = -3.0f; // 🚀 حالة جديدة: "جاري انتظار تحميل البوسترات" 
            }

            if (currentScreen.slideStartTime == -3.0f) {
                bool allReady = true;
                bool needsInternet = false;
                for (const auto& w : currentScreen.widgets) {
                    if (w.anim == "slide_up_fade" || w.anim == "slide_up") {
                        int checkCount = std::min(12, (int)w.items.size()); // نراقب أول 12 بوستر
                        for (int i = 0; i < checkCount; i++) {
                            if (!w.items[i].loaded && w.items[i].imagePath != "none" && w.items[i].imagePath != "loading_state" && !w.items[i].imagePath.empty()) {
                                allReady = false;
                                
                                // 🚀 الفحص السحري: هل الصورة من الإنترنت وليست في الكاش؟
                                std::string path = w.items[i].imagePath;
                                if (path.find("http://") == 0 || path.find("https://") == 0) {
                                    std::string localPath = url_to_filename(path);
                                    // access() تفحص وجود الملف في الهاردسك بسرعة خيالية (أجزاء من المايكروثانية)
                                    if (access(localPath.c_str(), F_OK) == -1) {
                                        needsInternet = true; // ⚠️ البوستر غير موجود! نحتاج إنترنت
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (needsInternet) break;
                }
                
                // 🚀 القرار الذكي: إذا كانت تحتاج إنترنت، لا ننتظر أبداً! نصعد فوراً وتظهر تدريجياً (2 بـ 2)
                // أما إذا كانت كلها في الكاش، ننتظرها لحظة لتصعد دفعة واحدة!
                if (allReady || needsInternet || (currentTime - animWaitStart > 1.5f)) {
                    currentScreen.slideStartTime = currentTime;
                }
            } else if (currentScreen.slideStartTime == -1.0f) {
                currentScreen.slideStartTime = currentTime; 
            }

            if (currentTime < currentScreen.slideStartTime || currentScreen.slideStartTime == -3.0f) {
                elapsedAnimTime = 0.0f; // 🚀 إبقاء الشاشة مختفية في الأسفل أثناء الانتظار 
            } else {
                elapsedAnimTime = currentTime - currentScreen.slideStartTime;
            }

            if (currentScreen.slideStartTime > 0.0f && elapsedAnimTime >= currentScreen.animDuration) {
                currentScreen.slideActive = false;
            }
        }

        // ✅ 2. دالة داخلية ذكية لحساب الإزاحة والوقت لكل عنصر بشكل مستقل تماماً
        // ==============================================================================
        // حساب إزاحة/شفافية الأنيميشن لعنصر واحد
        // ==============================================================================
        // ما تغيّر: كانت تستقبل std::string **بالقيمة** (نسختان لكل عنصر في كل إطار)
        // ثم تقارن السلسلة حتى 20 مرة. الآن مرجع ثابت + تحويل واحد إلى AnimId + switch.
        // ==============================================================================
        // تستقبل AnimId جاهزاً (محسوباً مرة واحدة في بداية الإطار) بدل إعادة
        // تجزئة السلسلة لكل عنصر في كل إطار.
        auto getElemAnimBase = [&](AnimId elemAnimId, AnimId screenAnimIdIn,
                               float elemDist, float elemDuration,
                               float& outX, float& outY, float& outAlpha) {
            outX = 0.0f; outY = 0.0f; outAlpha = 1.0f;
            const AnimId anim = (elemAnimId == AnimId::None) ? screenAnimIdIn : elemAnimId;
            if (anim == AnimId::None || anim == AnimId::Unknown) {
                if (!currentScreen.slideActive) return;
            }

            const bool isFinished = (!currentScreen.slideActive && currentScreen.slideStartTime >= 0.0f);

            if (isFinished) {
                switch (anim) {
                    case AnimId::SlideOutDown:  outY =  5000.0f;  outAlpha = 0.0f; return;
                    case AnimId::SlideOutLeft:  outX = -5000.0f;  outAlpha = 0.0f; return;
                    case AnimId::SlideOutRight: outX =  5000.0f;  outAlpha = 0.0f; return;
                    case AnimId::ShiftRight:     outX =  elemDist; outAlpha = 1.0f; return;
                    case AnimId::ShiftRightFade: outX =  elemDist; outAlpha = 1.0f; return;
                    case AnimId::ShiftDown:      outY =  elemDist; outAlpha = 1.0f; return;
                    case AnimId::ShiftDownFade:  outY =  elemDist; outAlpha = 1.0f; return;
                    case AnimId::FadeOut:        outAlpha = 0.0f; return;   // يبقى مخفياً بعد انتهائه
                    default: return;
                }
            }

            if (!currentScreen.slideActive) return;
            if (elemDuration <= 0.0f) elemDuration = 0.0001f;   // حماية من القسمة على صفر

            const float rawT  = fmin(1.0f, elapsedAnimTime / elemDuration);
            const float animT = glm_ease_out(currentScreen.easeId, rawT);   // 🎚️ Easing من الـ XML
            const float fade  = animT;

            switch (anim) {
                case AnimId::SlideUpFade:
                    outY = (1.0f - animT) * elemDist;  outAlpha = fade;            break;
                case AnimId::SlideDownFade:
                    outY = -(1.0f - animT) * elemDist; outAlpha = fade;            break;
                case AnimId::SlideUp:
                    outY = (1.0f - animT) * elemDist;  outAlpha = 1.0f;            break;
                case AnimId::SlideDown:
                    outY = -(1.0f - animT) * elemDist; outAlpha = 1.0f;            break;
                case AnimId::SlideOutDown:
                    outY = animT * elemDist;           outAlpha = 1.0f - fade;     break;
                case AnimId::SlideOutLeft:
                    outX = -(animT * elemDist);        outAlpha = 1.0f - fade;     break;
                case AnimId::SlideOutRight:
                    outX = (animT * elemDist);         outAlpha = 1.0f - fade;     break;
                case AnimId::ShiftRight:
                    outX = animT * elemDist;           outAlpha = 1.0f;            break;
                case AnimId::ShiftBackLeft:
                    outX = (1.0f - animT) * elemDist;  outAlpha = 1.0f;            break;
                case AnimId::ShiftDown:
                    outY = animT * elemDist;           outAlpha = 1.0f;            break;
                case AnimId::ShiftBackUp:
                    outY = (1.0f - animT) * elemDist;  outAlpha = 1.0f;            break;

                // 🌫️ الإصدارات المتلاشية.
                // fade هو animT نفسه (نفس easeOutQuint ونفس elemDuration)، لذلك
                // التلاشي يبدأ وينتهي مع الحركة في نفس اللحظة تماماً:
                //   دخول: الإزاحة تتقدّم من 0→elemDist والشفافية من 0→1
                //   خروج: الإزاحة تتراجع من elemDist→0 والشفافية من 1→0
                case AnimId::ShiftRightFade:
                    outX = animT * elemDist;           outAlpha = fade;            break;
                case AnimId::ShiftBackLeftFade:
                    outX = (1.0f - animT) * elemDist;  outAlpha = 1.0f - fade;     break;
                case AnimId::ShiftDownFade:
                    outY = animT * elemDist;           outAlpha = fade;            break;
                case AnimId::ShiftBackUpFade:
                    outY = (1.0f - animT) * elemDist;  outAlpha = 1.0f - fade;     break;

                // 🌫️ تلاشٍ خالص: لا إزاحة إطلاقاً، ونفس منحنى وتوقيت بقية العناصر
                case AnimId::FadeIn:
                    outAlpha = fade;                                               break;
                case AnimId::FadeOut:
                    outAlpha = 1.0f - fade;                                        break;
                case AnimId::MicroSlideLeft:
                    outX = (1.0f - animT) * 60.0f;     outAlpha = fade;            break;
                case AnimId::SlideRight:
                    outX = (1.0f - animT) * elemDist;  outAlpha = 1.0f;            break;
                case AnimId::SlideLeft:
                    outX = -(1.0f - animT) * elemDist; outAlpha = 1.0f;            break;
                case AnimId::SlideRightFade:
                    outX = (1.0f - animT) * elemDist;  outAlpha = fade;            break;
                case AnimId::SlideLeftFade:
                    outX = -(1.0f - animT) * elemDist; outAlpha = fade;            break;
                case AnimId::ZoomFade:
                    outAlpha = fade;                                               break;
                case AnimId::ZoomInFadeOut: {
                    const float animT2 = rawT * rawT;
                    outAlpha = 1.0f - animT2;
                    break;
                }
                default:
                    break;
            }
        };

        // ==============================================================================
        // 🌫️ الغلاف: يضيف التلاشي فوق نتيجة الحركة بلا أن يمسّها إطلاقاً
        // ==============================================================================
        // المطلوب: "يبدأ مع الحركة وينتهي معها في نفس اللحظة".
        // نحققه بحكم التعريف لا بالمعايرة: نفس elapsedAnimTime، ونفس elemDuration،
        // ونفس منحنى easeOutQuint الذي تستعمله الإزاحة — فيستحيل أن يسبق أحدهما الآخر.
        //
        // مفصول عن AnimId عمداً: كل أنيميشن موجود (shift_right، slide_up، none...)
        // يبقى على مساره المُختبَر حرفياً، والتلاشي مجرد مُضاعِف على outAlpha.
        // ==============================================================================
        auto getElemAnim = [&](AnimId elemAnimId, AnimId screenAnimIdIn,
                               float elemDist, float elemDuration, int fadeMode,
                               float& outX, float& outY, float& outAlpha) {
            getElemAnimBase(elemAnimId, screenAnimIdIn, elemDist, elemDuration, outX, outY, outAlpha);
            if (fadeMode == 0) return;

            float ramp;
            if (!currentScreen.slideActive) {
                ramp = 1.0f;                     // الأنيميشن انتهى ⇒ الحالة النهائية
            } else {
                const float d    = (elemDuration <= 0.0f) ? 0.0001f : elemDuration;
                const float rawT = fmin(1.0f, elapsedAnimTime / d);
                ramp = glm_ease_out(currentScreen.easeId, rawT);   // 🎚️ نفس منحنى الحركة تماماً
            }
            outAlpha *= (fadeMode == 1) ? ramp : (1.0f - ramp);
        };

        // ==========================================================
        // ✅ 1. رسم الـ Backdrop في الإحداثيات المحددة
        // ==========================================================
       
        

        float bgAlpha = 1.0f;
        if (currentScreen.isBgFading) {
            if (currentScreen.bgFadeStartTime < 0.0f) currentScreen.bgFadeStartTime = currentTime;
            float elapsed = currentTime - currentScreen.bgFadeStartTime;
            bgAlpha = (currentScreen.bgFadeDuration > 0.001f)
                      ? (elapsed / currentScreen.bgFadeDuration)
                      : 1.0f;   // fadeSpeed="0" ⇒ تبديل فوري بلا إطارات ثقيلة
            
            if (bgAlpha >= 1.0f) {
                bgAlpha = 1.0f;
                currentScreen.isBgFading = false;
                if (currentScreen.oldBg.textureId != 0) {
                    glDeleteTextures(1, &currentScreen.oldBg.textureId);
                    currentScreen.oldBg.textureId = 0;
                }
                // ✅ السطر السحري المفقود: يخبر المعالج بإيقاف الشفافية نهائياً!
                currentScreen.oldBg.loaded = false; 
            }
        }

        float bX = currentScreen.backdropX;
        float bY = currentScreen.backdropY;
        float bW = currentScreen.backdropW;
        float bH = currentScreen.backdropH;
        float backdropV[] = { bX, bY, bX + bW, bY, bX, bY + bH, bX + bW, bY + bH };
        //float overlayDarkness = 0.4f;
        float overlayDarkness = 1.0f;
        // 🚀 إضافة هذا السطر الجديد لربط الخلفية بكاميرا الزووم الشاملة
        const glm::mat4& bgProj = anim_uses_zoom_camera(screenAnimId) ? animProjection : baseProjection;

        // 🚀 Trailer Exit Cover: يرسم الباكدروب الحالي كغطاء تحت الواجهة وفوق طبقة الفيديو،
        // حتى لا تظهر لحظة سواد عند stopService. يرسم قبل الباكدروب العادي والـ UI.
        if (coverAlpha > 0.001f && currentScreen.currentBg.loaded && currentScreen.currentBg.textureId != 0) {
            gl_set_blend(true);
            gl_use_program(fastProgram);
            glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(bgProj));
            glUniform1f(fastUseTexLoc, 1.0f);
            glUniform4f(fastColorLoc, overlayDarkness, overlayDarkness, overlayDarkness, coverAlpha);
            glBindTexture(GL_TEXTURE_2D, currentScreen.currentBg.textureId);
            glEnableVertexAttribArray(fastPosLoc); glEnableVertexAttribArray(fastTexCoordLoc);
            glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, backdropV);
            glVertexAttribPointer(fastTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        // 👇 رسم الباكدروب مع تحسينات توفير طاقة كرت الشاشة
        if (drawStaticBaseNow && trailerAlpha > 0.0f) {
        // 🚀 توفير: متى صار الجديد معتماً عملياً (>92%) لا معنى لرسم القديم تحته
        if (currentScreen.oldBg.loaded && currentScreen.oldBg.textureId != 0 && bgAlpha < 0.92f) {
            // 🚀 كفاءة قصوى: تفعيل الشفافية فقط وقت الفيد لإنقاذ الـ Fill-rate
            if (trailerAlpha < 1.0f) gl_set_blend(true); else gl_set_blend(false);
            gl_use_program(fastProgram);
            glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(bgProj));
            glUniform1f(fastUseTexLoc, 1.0f);
            glUniform4f(fastColorLoc, overlayDarkness, overlayDarkness, overlayDarkness, trailerAlpha);
            glBindTexture(GL_TEXTURE_2D, currentScreen.oldBg.textureId);
            glEnableVertexAttribArray(fastPosLoc); glEnableVertexAttribArray(fastTexCoordLoc);
            glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, backdropV);
            glVertexAttribPointer(fastTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        if (currentScreen.currentBg.loaded && currentScreen.currentBg.textureId != 0) {
            // ✅ تفعيل الـ Blend أثناء تلاشي الخلفية أو أثناء أنيميشن الشاشة الكاملة لضمان النعومة
            gl_set_blend(true); // 👈 تفعيل الشفافية دائماً هنا ليسمح بالفيد
            
            gl_use_program(fastProgram);
            glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(bgProj));
            glUniform1f(fastUseTexLoc, 1.0f);
            glUniform4f(fastColorLoc, overlayDarkness, overlayDarkness, overlayDarkness, bgAlpha * screenAlpha * trailerAlpha); // 👈 دمج جميع الشفافيات
            glBindTexture(GL_TEXTURE_2D, currentScreen.currentBg.textureId);
            glEnableVertexAttribArray(fastPosLoc); glEnableVertexAttribArray(fastTexCoordLoc);
            glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, backdropV);
            glVertexAttribPointer(fastTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        }
        // ==========================================================
        // ✅ رسم التدرجات اللونية (Gradients) برمجياً
        // ==========================================================
        if (drawStaticBaseNow && !currentScreen.gradients.empty()) {
            gl_use_program(gradProgram);
            gl_set_blend(true);
            glUniformMatrix4fv(gradProjLoc, 1, GL_FALSE, glm::value_ptr(baseProjection));
            
            for (const auto& gr : currentScreen.gradients) {
                float elemOffsetX, elemOffsetY, elemAlpha;
                getElemAnim(gr.animId, screenAnimId, gr.animDistance, gr.animDuration, gr.fadeMode, elemOffsetX, elemOffsetY, elemAlpha);

                glUniform4f(gradColorStartLoc, gr.r1, gr.g1, gr.b1, gr.a1 * elemAlpha);
                glUniform4f(gradColorEndLoc, gr.r2, gr.g2, gr.b2, gr.a2 * elemAlpha);
                
                float gx = gr.x + elemOffsetX; 
                float gy = gr.y + elemOffsetY;
                float v[] = { gx, gy, gx + gr.w, gy, gx, gy + gr.h, gx + gr.w, gy + gr.h };
                glVertexAttribPointer(gradPosLoc, 2, GL_FLOAT, GL_FALSE, 0, v);
                glEnableVertexAttribArray(gradPosLoc);
                glVertexAttribPointer(gradTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
                glEnableVertexAttribArray(gradTexCoordLoc);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
        }

        // ==========================================================
        // ✅ دالة رسم الصور (نظام Z-Sorting الديناميكي الموحد)
        // ==========================================================
        auto draw_images_layer = [&](int currentZ) {
            for (auto& img : currentScreen.images) {
                if (img.z != currentZ) continue;
                
                float elemOffsetX, elemOffsetY, elemAlpha;
                getElemAnim(img.animId, screenAnimId, img.animDistance, img.animDuration, img.fadeMode, elemOffsetX, elemOffsetY, elemAlpha);
                elemAlpha *= screenAlpha; // 🚀 تطبيق التلاشي الشامل للشاشة بأكملها

                // 🚀 منطق التريلر:
                // mask_normal تبقى ظاهرة دائماً فوق الفيديو/الباكدروب.
                // mask_trailer تظهر/تختفي فوراً بدون fade حسب حالة التريلر فقط.
                if (img.name == "mask_normal") {
                    if (!current_bh && !img.adaptiveTint) continue;   // 🎨 التكيّفي يُرسم دائماً
                } else if (img.name == "mask_trailer") {
                    if (!current_bh) continue;
                }
                
                // 🚀 السحر هنا: تحديد الكاميرا بناءً على العنصر (إذا كان none يظل ثابتاً تماماً)
                // بلا تخصيص ذاكرة: المعرّف الرقمي جاهز من بداية الإطار
                const AnimId activeAnim = img.anim.empty() ? screenAnimId : img.animId;
                const glm::mat4& activeProj =
                    (anim_uses_zoom_camera(activeAnim) && img.animId != AnimId::None)
                    ? animProjection : baseProjection;

                // ==========================================================
                // 🎨 Adaptive Background Color
                // ==========================================================
                // نأخذ اللون المهيمن لبوستر العنصر المحدَّد في الويدجت المصدر،
                // ننتقل إليه بنعومة، ثم نرسم الـ PNG بوضع "التلوين بالألفا":
                // الشكل والتدرّج من الصورة، واللون من البوستر.
                if (img.adaptiveTint && img.loaded && img.textureId != 0) {
                    // اللون محسوب مسبقاً في بداية الإطار — هنا رسم فقط.
                    gl_set_blend(true);
                    gl_use_program(program);
                    glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(activeProj));
                    glUniform1f(gradientModeLoc, 0.0f);
                    glUniform1f(roundLoc, 0.0f);
                    glUniform1f(borderWidthLoc, 0.0f);
                    glUniform1f(useTexLoc, 2.0f);          // 🎨 وضع التلوين بالألفا
                    glBindTexture(GL_TEXTURE_2D, img.textureId);
                    glUniform4f(colorLoc, img.adpCurR, img.adpCurG, img.adpCurB, 1.0f * elemAlpha);
                    float ax = img.x + elemOffsetX, ay = img.y + elemOffsetY;
                    float av[] = { ax, ay, ax + img.w, ay, ax, ay + img.h, ax + img.w, ay + img.h };
                    glEnableVertexAttribArray(posLoc); glEnableVertexAttribArray(texCoordLoc);
                    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, av);
                    glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glUniform1f(useTexLoc, 1.0f);          // إعادة الوضع الافتراضي
                    continue;
                }

                if (img.loaded && img.textureId != 0) {
                    if (img.cornerRadius <= 0.0f) {
                        // ✅ تفعيل Blend ديناميكياً
                        // ✅ السطر الصحيح: لا تفعل الشفافية إلا إذا كانت الصورة حقاً تحتوي على شفافية
                        if (img.hasAlpha || elemAlpha < 1.0f) gl_set_blend(true);
                        else gl_set_blend(false);
                        gl_use_program(fastProgram); 
                        glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(activeProj)); // 👈 تمرير الكاميرا
                        glUniform1f(fastUseTexLoc, 1.0f);
                        glUniform4f(fastColorLoc, 1.0f, 1.0f, 1.0f, 1.0f * elemAlpha); 
                        glBindTexture(GL_TEXTURE_2D, img.textureId);
                        glEnableVertexAttribArray(fastPosLoc); glEnableVertexAttribArray(fastTexCoordLoc);
                        float x1 = img.x + elemOffsetX; float y1 = img.y + elemOffsetY;
                        float imgV[] = { x1, y1, x1+img.w, y1, x1, y1+img.h, x1+img.w, y1+img.h };
                        glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, imgV);
                        glVertexAttribPointer(fastTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    } 
                    else {
                        gl_set_blend(true);
                        gl_use_program(program);
                        glUniform1f(gradientModeLoc, 0.0f); // 🚀 أضف هذا السطر هنا لحماية البوسترات
                        glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(activeProj));
                        float x1 = img.x + elemOffsetX; float y1 = img.y + elemOffsetY;
                        glUniform1f(roundLoc, img.cornerRadius);
                        glUniform2f(rectPosLoc, x1, y1);
                        glUniform2f(boxSizeLoc, img.w, img.h);
                        glUniform1f(borderWidthLoc, 0.0f);
                        glUniform1f(useTexLoc, 1.0f);
                        glBindTexture(GL_TEXTURE_2D, img.textureId);
                        glUniform4f(colorLoc, 1.0f, 1.0f, 1.0f, 1.0f * elemAlpha);
                        float v[] = { x1, y1, x1+img.w, y1, x1, y1+img.h, x1+img.w, y1+img.h };
                        glEnableVertexAttribArray(posLoc); glEnableVertexAttribArray(texCoordLoc);
                        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, v);
                        glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    }
                }
            }
        };

        // ==========================================================
        // ✅ دالة رسم النصوص والمربعات (نظام Z-Sorting الديناميكي الموحد)
        // ==========================================================
        auto draw_labels_layer = [&](int currentZ) {
            for (auto& lbl : currentScreen.labels) {
                if (lbl.z != currentZ) continue;
                gl_set_blend(true);
                float elemOffsetX, elemOffsetY, elemAlpha;
                getElemAnim(lbl.animId, screenAnimId, lbl.animDistance, lbl.animDuration, lbl.fadeMode, elemOffsetX, elemOffsetY, elemAlpha);
                elemAlpha *= screenAlpha; // 🚀 تطبيق التلاشي الشامل للنصوص

                // 🚀 تحديد الكاميرا للنصوص والمربعات
                const AnimId activeAnim = lbl.anim.empty() ? screenAnimId : lbl.animId;
                const glm::mat4& activeProj =
                    (anim_uses_zoom_camera(activeAnim) && lbl.animId != AnimId::None)
                    ? animProjection : baseProjection;
                
                gl_use_program(program);
                glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(activeProj)); // 👈 تمرير الكاميرا
                // السماح بشفافية الخلفية والإطار في كل أنواع الأنيميشن بشكل طبيعي
                float bgAlphaFix = lbl.bgA * elemAlpha;
                float borderAlphaFix = lbl.borderA * elemAlpha;
                
                // منع نصف القطر من تجاوز نصف الارتفاع أو العرض لتفادي تشوه الشادر
                float maxAllowedRadius = std::min(lbl.w, lbl.h) / 2.0f;
                float safeRadius = std::min(lbl.cornerRadius, maxAllowedRadius);
                glUniform1f(roundLoc, safeRadius);
                glUniform2f(rectPosLoc, lbl.x + elemOffsetX, lbl.y + elemOffsetY);
                glUniform2f(boxSizeLoc, lbl.w, lbl.h);

                if (lbl.hasBorder) {
                    glUniform1f(gradientModeLoc, (float)lbl.borderGradientMode);
                    if (lbl.borderGradientMode > 0) {
                        glUniform4f(colorEndLoc, lbl.borderR2, lbl.borderG2, lbl.borderB2, lbl.borderA2 * elemAlpha);
                    }
                    gl_set_blend(true);
                    glUniform1f(useTexLoc, 0.0f);
                    glUniform4f(colorLoc, lbl.borderR, lbl.borderG, lbl.borderB, borderAlphaFix);
                    glUniform1f(borderWidthLoc, lbl.borderWidth);
                    float bx = lbl.x + elemOffsetX; float by = lbl.y + elemOffsetY;
                    float v[] = { bx, by, bx + lbl.w, by, bx, by + lbl.h, bx + lbl.w, by + lbl.h };
                    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, v); 
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glUniform1f(gradientModeLoc, 0.0f); // 🚀 تصفير التدرج بعد رسم الإطار للحماية
                    gl_set_blend(true);
                }

                if (lbl.bgA > 0.0f || lbl.gradientMode > 0) {
                    gl_set_blend(true);
                    float offset = lbl.hasBorder ? lbl.borderWidth : 0.0f;
                    float innerX = lbl.x + elemOffsetX + offset;
                    float innerY = lbl.y + elemOffsetY + offset;
                    float innerW = lbl.w - (offset * 2.0f);
                    float innerH = lbl.h - (offset * 2.0f);
                    float innerRadius = lbl.cornerRadius - offset;
                    if (innerRadius < 0.0f) innerRadius = 0.0f;
                    float bv[] = { innerX, innerY, innerX + innerW, innerY, innerX, innerY + innerH, innerX + innerW, innerY + innerH };

                    if (innerRadius <= 0.0f && !lbl.hasBorder && lbl.gradientMode == 0) {
                        gl_use_program(fastProgram);
                        glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(activeProj)); // 👈 تمرير الكاميرا السريعة
                        glUniform1f(fastUseTexLoc, 0.0f);
                        glUniform4f(fastColorLoc, lbl.bgR, lbl.bgG, lbl.bgB, bgAlphaFix);
                        glEnableVertexAttribArray(fastPosLoc);
                        glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, bv);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        
                        // إعادة ضبط المحرك الأساسي
                        gl_use_program(program);
                        glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(activeProj)); // 👈 إعادة الكاميرا الأساسية 
                        glEnableVertexAttribArray(texCoordLoc);
                    } else {
                        glUniform1f(useTexLoc, 0.0f);
                        glUniform4f(colorLoc, lbl.bgR, lbl.bgG, lbl.bgB, bgAlphaFix);
                        glUniform1f(gradientModeLoc, (float)lbl.gradientMode);
                        if (lbl.gradientMode > 0) {
                            glUniform4f(colorEndLoc, lbl.bgR2, lbl.bgG2, lbl.bgB2, lbl.bgA2 * elemAlpha);
                        }
                        glUniform1f(roundLoc, innerRadius);
                        glUniform2f(rectPosLoc, innerX, innerY);
                        glUniform2f(boxSizeLoc, innerW, innerH);
                        glUniform1f(borderWidthLoc, 0.0f);
                        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, bv);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        glUniform1f(gradientModeLoc, 0.0f);
                    }
                    gl_set_blend(true);
                }

                if (!lbl.text.empty() && lbl.textureId != 0) {
                    glUniform1f(roundLoc, 0.0f);
                    glUniform1f(useTexLoc, 1.0f); 
                    glBindTexture(GL_TEXTURE_2D, lbl.textureId);
                    glUniform1f(borderWidthLoc, 0.0f); // 🚀 السحر هنا: إخبار كرت الشاشة بإيقاف القص قبل رسم النص! 
                    glUniform4f(colorLoc, lbl.r, lbl.g, lbl.b, 1.0f * elemAlpha);
                    
                    float drawW = lbl.texW > 0 ? lbl.texW : lbl.w;
                    // ✅ الإصلاح السحري: استخدام texH بدلاً من tightH لمنع الانكماش
                    float drawH = lbl.texH > 0 ? lbl.texH : lbl.h; 
                    
                    float textX = lbl.x + elemOffsetX;
                    float textY = lbl.y + elemOffsetY + (lbl.h - drawH) / 2.0f;
                    
                    if (lbl.useManualTextPos) { textX = lbl.x + lbl.manualTextX + elemOffsetX; textY = lbl.y + lbl.manualTextY + elemOffsetY; } 
                    else if (lbl.alignId == AlignId::Center) { textX = lbl.x + (lbl.w - drawW) / 2.0f + elemOffsetX; } 
                    else if (lbl.alignId == AlignId::Right) { textX = lbl.x + lbl.w - drawW + elemOffsetX; }

                    float startX = textX;
                    float startY = textY;
                    
                    if (lbl.scrollTextMode == 1 && drawW > lbl.w) {
                        if (lbl.textScrollActive) {
                            if (lbl.textScrollStartTime < 0.0f) lbl.textScrollStartTime = currentTime;
                            float elapsed = currentTime - lbl.textScrollStartTime;
                            float delay = lbl.scrollDelay >= 0.0f ? lbl.scrollDelay : 2.0f;
                            if (elapsed > delay) {
                                float activeTime = elapsed - delay;
                                float speed = 120.0f;
                                float max_scroll_dist = drawW - lbl.w;

                                if (lbl.scrollTextStyle == 2) { // 🚀 نمط الارتداد ذهاب وعودة
                                    float T_scroll = max_scroll_dist / speed;
                                    float T_pause = 1.0f; // وقفة مريحة لمدة ثانية عند الاكتمال
                                    float T_cycle = T_scroll + T_pause + T_scroll + T_pause;
                                    int current_loop = (int)(activeTime / T_cycle);

                                    if (current_loop >= lbl.scrollTextShot) {
                                        lbl.textScrollActive = false;
                                        textX = startX;
                                    } else {
                                        float time_in_cycle = fmod(activeTime, T_cycle);
                                        if (time_in_cycle < T_scroll) {
                                            textX = startX - (time_in_cycle * speed); // حركة لليسار
                                        } else if (time_in_cycle < T_scroll + T_pause) {
                                            textX = startX - max_scroll_dist; // ثبات عند النهاية
                                        } else if (time_in_cycle < T_scroll + T_pause + T_scroll) {
                                            float t_rev = time_in_cycle - (T_scroll + T_pause);
                                            textX = (startX - max_scroll_dist) + (t_rev * speed); // عودة لليمين
                                        } else {
                                            textX = startX; // ثبات عند البداية قبل الدورة القادمة
                                        }
                                    }
                                } else { // 🔄 النمط 1 الافتراضي (دوران مستمر من الحافة للحافة)
                                    float d1 = startX - (lbl.x + elemOffsetX) + drawW;
                                    float L = drawW + lbl.w;
                                    float D = fmod(activeTime * speed, L);
                                    int loops = (int)((activeTime * speed) / L);
                                    if (loops >= lbl.scrollTextShot) {
                                        lbl.textScrollActive = false;
                                        textX = startX;
                                    } else {
                                        if (D < d1) textX = startX - D;
                                        else textX = ((lbl.x + elemOffsetX) + lbl.w) - (D - d1);
                                    }
                                }
                            }
                        }
                    } else if (lbl.scrollTextMode == 2) {
                        if (lbl.textScrollActive) {
                            if (lbl.textScrollStartTime < 0.0f) lbl.textScrollStartTime = currentTime;
                            float elapsed = currentTime - lbl.textScrollStartTime;
                            float delay = lbl.scrollDelay >= 0.0f ? lbl.scrollDelay : 0.5f;
                            if (elapsed > delay) {
                                float activeTime = elapsed - delay;
                                float speed = 60.0f; // سرعة الصعود
                                float boxY = lbl.y + elemOffsetY;
                                float d1_y = startY - boxY + drawH;
                                float L_y = drawH + lbl.h;
                                float D_y = fmod(activeTime * speed, L_y);
                                int loops = (int)((activeTime * speed) / L_y);
                                if (loops >= 1) { // دورة واحدة فقط للاستقرار
                                    lbl.textScrollActive = false;
                                    textY = startY;
                                } else {
                                    if (D_y < d1_y) textY = startY - D_y;
                                    else textY = (boxY + lbl.h) - (D_y - d1_y);
                                }
                            }
                        }
                    }
                    
                    glEnable(GL_SCISSOR_TEST);
                    glScissor((int)(lbl.x + elemOffsetX), h - (int)(lbl.y + elemOffsetY + lbl.h), (int)lbl.w, (int)lbl.h);
                    
                    float v[] = { textX, textY, textX + drawW, textY, textX, textY + drawH, textX + drawW, textY + drawH };
                    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, v); 
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    
                    glDisable(GL_SCISSOR_TEST);
                } else if (lbl.text.empty() && lbl.iconTextureId != 0) {
                    glUniform1f(roundLoc, 0.0f);
                    glUniform1f(useTexLoc, 1.0f); 
                    glBindTexture(GL_TEXTURE_2D, lbl.iconTextureId);
                    glUniform1f(borderWidthLoc, 0.0f); // 🚀 وإيقاف القص هنا أيضاً للأيقونات! 
                    glUniform4f(colorLoc, lbl.r, lbl.g, lbl.b, 1.0f * elemAlpha);
                    
                    float iconX = lbl.x + elemOffsetX + (lbl.w - lbl.iconTexW) / 2.0f;
                    float iconY = lbl.y + elemOffsetY + (lbl.h - lbl.iconTexH) / 2.0f;
                    
                    float iv[] = { iconX, iconY, iconX + lbl.iconTexW, iconY, iconX, iconY + lbl.iconTexH, iconX + lbl.iconTexW, iconY + lbl.iconTexH };
                    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, iv);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
            }
        };

        // ==========================================================
        // 🚀 ترتيب التنفيذ الفعلي (Layer Execution) بنظام zPosition الموحد
        // ==========================================================

        std::vector<int> zLayers;
        for (const auto& img : currentScreen.images) zLayers.push_back(img.z);
        for (const auto& lbl : currentScreen.labels) zLayers.push_back(lbl.z);
        for (const auto& w : currentScreen.widgets) zLayers.push_back(w.z);
        
        std::sort(zLayers.begin(), zLayers.end());
        zLayers.erase(std::unique(zLayers.begin(), zLayers.end()), zLayers.end());
        if (zLayers.empty()) zLayers.push_back(0);

        if (layeredCacheActive && !buildLayerCacheThisFrame) {
            // الخلفية/البوسترات جاهزة في FBO: اعرضها أولاً ثم ارسم الطبقات العليا فقط.
            draw_layer_scene_cache_to_screen(w, h);
        }

        bool layerCachePresented = (!layeredCacheActive || !buildLayerCacheThisFrame);
        bool staticCachePresented = false; // 🚀 متغير جديد

        for (int currentZ : zLayers) {
            // 🚀 السحر الجديد: إغلاق الكاش الثابت مبكراً قبل رسم طبقات القوائم (Z=6 فما فوق)
            // لتبقى القوائم العلوية دائماً مرسومة بشكل حي (Live) ولا تهتز أبداً!
            if (buildStaticCacheThisFrame && currentZ >= 6 && !staticCachePresented) {
                glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo); gl_state_cache_reset();
                staticSceneValid = true;
                staticSceneDirty.store(false);
                draw_static_scene_cache_to_screen(w, h);
                staticCachePresented = true;

                // إعادة تفعيل الكاميرا الأساسية للرسم الحي
                gl_use_program(program);
                glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(baseProjection));
                glEnableVertexAttribArray(posLoc);
                glEnableVertexAttribArray(texCoordLoc);
                glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
            }
            if (layeredCacheActive) {
                if (currentZ < dynamicMinZ) {
                    // أثناء بناء الكاش: الطبقات الأقل ترسم داخل الـ FBO.
                    // أثناء استعمال كاش جاهز: نتخطاها لأنها موجودة مسبقاً في الصورة المجمدة.
                    if (!buildLayerCacheThisFrame) continue;
                } else if (buildLayerCacheThisFrame && !layerCachePresented) {
                    // وصلنا لأول طبقة متحركة/عليا: أغلق FBO واعرض اللقطة ثم أكمل على الشاشة.
                    glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo); gl_state_cache_reset();
                    layerSceneValid = true;
                    layerSceneDirty.store(false);
                    draw_layer_scene_cache_to_screen(w, h);
                    layerCachePresented = true;
                    gl_use_program(program);
                    glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(baseProjection));
                    glEnableVertexAttribArray(posLoc);
                    glEnableVertexAttribArray(texCoordLoc);
                    glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
                }
            }
            
            draw_images_layer(currentZ);

            bool hasLabels = false;
            for (const auto& lbl : currentScreen.labels) { if (lbl.z == currentZ) { hasLabels = true; break; } }
            if (hasLabels) {
                gl_use_program(program);
                gl_set_blend(true);
                glEnableVertexAttribArray(posLoc); 
                glEnableVertexAttribArray(texCoordLoc);
                glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
                draw_labels_layer(currentZ);
            }

            bool hasWidgets = false;
            for (const auto& w : currentScreen.widgets) { if (w.z == currentZ) { hasWidgets = true; break; } }
            if (hasWidgets) {
                gl_use_program(program);
                glEnableVertexAttribArray(posLoc); 
                glEnableVertexAttribArray(texCoordLoc);
                glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);

                for (auto& w : currentScreen.widgets) {
                    if (w.z != currentZ) continue;
                    // 👇 أضف هذا السطر هنا لتخطي رسم الويدجت وخلفيته إذا كان فارغاً
                    //if (w.items.empty()) continue;

                    // 🚀 استكمال توليد النصوص المتبقية إطاراً بعد إطار حتى تكتمل
                    //    كل النصوص المرئية بلا انتظار ضغطة زر جديدة.
                    if (w.textPending && !w.items.empty()) {
                        manage_widget_textures(w);
                    }
                    
                    // 🚀 تحديد الكاميرا للويدجت
                    const AnimId activeAnim = w.anim.empty() ? screenAnimId : w.animId;
                    const glm::mat4& activeProj =
                        (anim_uses_zoom_camera(activeAnim) && w.animId != AnimId::None)
                        ? animProjection : baseProjection;
                    glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(activeProj)); // 👈 تمرير الكاميرا

                    float elemOffsetX, elemOffsetY, elemAlpha;
                    getElemAnim(w.animId, screenAnimId, w.animDistance, w.animDuration, w.fadeMode, elemOffsetX, elemOffsetY, elemAlpha);
                    // 🚀 حساب الأنيميشن المستقل (Local Anim) الخاص بالويدجت (للأعداد)
                    float locOffsetX = 0.0f, locOffsetY = 0.0f, locAlpha = 1.0f;
                    if (w.localAnimActive) {
                        if (w.localAnimStartTime < 0.0f) {
                            bool allLoaded = true;
                            if (w.name == "poster_row" && !w.items.empty()) {
                                size_t checkCount = w.items.size() > 8 ? 8 : w.items.size();
                                for (size_t k = 0; k < checkCount; ++k) {
                                    if (!w.items[k].loaded) {
                                        allLoaded = false;
                                        break;
                                    }
                                }
                            }
                            if (w.textPending) {
                                allLoaded = false;
                            }
                            if (allLoaded) {
                                w.localAnimStartTime = currentTime;
                            } else {
                                locAlpha = 0.0f; // hide until loaded
                            }
                        }
                        
                        if (w.localAnimStartTime >= 0.0f) {
                            float elapsed = currentTime - w.localAnimStartTime;
                            if (elapsed >= w.localAnimDuration) {
                            w.localAnimActive = false;
                        } else {
                            float rawT = fmin(1.0f, elapsed / w.localAnimDuration);
                            // ==========================================================
                            // 🚀 منحنى واحد للحركة والشفافية معاً
                            // ==========================================================
                            // كان الموضع يستعمل easeOutQuint بينما الشفافية تستعمل rawT
                            // الخطي في أنيميشنات الدخول. النتيجة أن الانزلاق يكتمل عند
                            // ~50% من المدة (96.9% من المسافة) بينما الشفافية لم تتجاوز
                            // 50% بعد، فتُرى الحركة وقد توقفت والعنصر ما زال يصعد وضوحاً.
                            // استعمال animT للاثنين يجعلهما دالة واحدة في الزمن، فينتهيان
                            // في نفس اللحظة بحكم التعريف لا بالمعايرة.
                            //
                            // لتنعيم الحركة والتلاشي معاً (إن بدا الظهور سريعاً): خفّض
                            // الأس هنا من 5 إلى 3 — يبقيان متزامنين لأنهما يتقاسمان animT.
                            float invT = 1.0f - rawT;
                            float animT = 1.0f - (invT * invT * invT * invT * invT); // easeOutQuint

                            if (w.localAnimType == "slide_up_fade") {
                                locOffsetY = (1.0f - animT) * w.localAnimDistance;
                                locAlpha = animT; // 🚀 نفس منحنى الحركة تماماً
                            } else if (w.localAnimType == "slide_down_fade_out") {
                                locOffsetY = animT * w.localAnimDistance;
                                locAlpha = 1.0f - animT; // 🚀 خروج: عاد كما كان سابقاً
                            } else if (w.localAnimType == "slide_left_fade") { // دخول من اليمين
                                locOffsetX = (1.0f - animT) * w.localAnimDistance;
                                locAlpha = animT; // 🚀 نفس منحنى الحركة تماماً
                            } else if (w.localAnimType == "slide_right_fade_out") { // خروج لليمين
                                locOffsetX = animT * w.localAnimDistance;
                                locAlpha = 1.0f - animT; // 🚀 خروج: عاد كما كان سابقاً ليظهر بشكل ممتاز
                            }
                            // 👇 أضف هذه الأسطر الجديدة هنا 👇
                            else if (w.localAnimType == "slide_right_fade") { // دخول من اليسار
                                locOffsetX = -(1.0f - animT) * w.localAnimDistance;
                                locAlpha = animT; // 🚀 نفس منحنى الحركة تماماً
                            } else if (w.localAnimType == "slide_left_fade_out") { // خروج لليسار
                                locOffsetX = -animT * w.localAnimDistance;
                                locAlpha = 1.0f - animT;
                            } else if (w.localAnimType == "slide_left") { // دخول من اليمين بدون تلاشي
                                locOffsetX = (1.0f - animT) * w.localAnimDistance;
                                locAlpha = 1.0f;
                            } else if (w.localAnimType == "slide_right") { // خروج لليمين بدون تلاشي
                                locOffsetX = animT * w.localAnimDistance;
                                locAlpha = 1.0f;    
                            //
                            }
                        }
                        }
                    }

                    //
                    if (w.items.empty()) continue;

                    // دمج الأنيميشن
                    elemOffsetX += locOffsetX;
                    elemOffsetY += locOffsetY;
                    elemAlpha *= locAlpha;
                    elemAlpha *= screenAlpha; // 🚀 تطبيق التلاشي الشامل للقوائم
            // ✅ رسم الحاوية الخلفية للويدجت (مع دعم الإطار والحواف الدائرية)
            float bgAlphaFix = w.bgA * elemAlpha;
            float borderAlphaFix = w.borderA * elemAlpha;
            
            float safeRadius = std::min(w.cornerRadius, std::min(w.w, w.h) / 2.0f);
            glUniform1f(roundLoc, safeRadius);
            glUniform2f(rectPosLoc, w.x + elemOffsetX, w.y + elemOffsetY);
            glUniform2f(boxSizeLoc, w.w, w.h);

            if (w.hasBorder) {
                glUniform1f(gradientModeLoc, (float)w.borderGradientMode);
                if (w.borderGradientMode > 0) {
                    glUniform4f(colorEndLoc, w.borderR2, w.borderG2, w.borderB2, w.borderA2 * elemAlpha);
                }
                gl_set_blend(true);
                glUniform1f(useTexLoc, 0.0f);
                glUniform4f(colorLoc, w.borderR, w.borderG, w.borderB, borderAlphaFix);
                glUniform1f(borderWidthLoc, w.borderWidth);
                float bx = w.x + elemOffsetX; float by = w.y + elemOffsetY;
                float v[] = { bx, by, bx + w.w, by, bx, by + w.h, bx + w.w, by + w.h };
                glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, v); 
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glUniform1f(gradientModeLoc, 0.0f); // 🚀 تصفير التدرج بعد رسم الإطار للحماية
                gl_set_blend(true);
            }

            // ✅ الكود الصحيح: إطفاء الشفافية إذا كان اللون أسود 100% معتم وبدون زوايا دائرية
            if (w.bgA > 0.0f || w.gradientMode > 0) {
                if (bgAlphaFix < 1.0f || w.cornerRadius > 0.0f) {
                    gl_set_blend(true);
                } else {
                    gl_set_blend(false);
                }
                float offset = w.hasBorder ? w.borderWidth : 0.0f;
                float innerX = w.x + elemOffsetX + offset;
                float innerY = w.y + elemOffsetY + offset;
                float innerW = w.w - (offset * 2.0f);
                float innerH = w.h - (offset * 2.0f);
                float innerRadius = w.cornerRadius - offset;
                if (innerRadius < 0.0f) innerRadius = 0.0f;
                
                float bv[] = { innerX, innerY, innerX + innerW, innerY, innerX, innerY + innerH, innerX + innerW, innerY + innerH };

                if (innerRadius <= 0.0f && !w.hasBorder && w.gradientMode == 0) {
                    gl_use_program(fastProgram);
                    glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(activeProj));
                    glUniform1f(fastUseTexLoc, 0.0f);
                    glUniform4f(fastColorLoc, w.bgR, w.bgG, w.bgB, bgAlphaFix);
                    glEnableVertexAttribArray(fastPosLoc);
                    glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, bv);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    
                    gl_use_program(program);
                    glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(activeProj));
                    glEnableVertexAttribArray(texCoordLoc);
                } else {
                    glUniform1f(useTexLoc, 0.0f);
                    glUniform4f(colorLoc, w.bgR, w.bgG, w.bgB, bgAlphaFix);
                    glUniform1f(gradientModeLoc, (float)w.gradientMode);
                    if (w.gradientMode > 0) {
                        glUniform4f(colorEndLoc, w.bgR2, w.bgG2, w.bgB2, w.bgA2 * elemAlpha);
                    }
                    glUniform1f(roundLoc, innerRadius);
                    glUniform2f(rectPosLoc, innerX, innerY);
                    glUniform2f(boxSizeLoc, innerW, innerH);
                    glUniform1f(borderWidthLoc, 0.0f);
                    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, bv);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glUniform1f(gradientModeLoc, 0.0f);
                }
                gl_set_blend(true);
            }

            // 🚀 مهم: Prime-style لا يطبق على كل Widgets لأن المحرك مشترك بين بلوجينات كثيرة.
            // نفعّله فقط لكاروسال بوسترات أفقي يحتوي صوراً حقيقية.
            bool primeCarouselMode = false;
            if (w.orientId == OrientId::Horizontal && w.itemW >= 80.0f && w.itemH >= 80.0f) {
                for (const auto& it : w.items) {
                    if (!it.imagePath.empty() && it.imagePath != "none" && it.imagePath != "loading_state") {
                        primeCarouselMode = true; break;
                    }
                }
            }

            float tCarousel = 1.0f;
            float tZoom = 1.0f;
            float tSelection = 1.0f;
            if (w.isAnimating) {
                if (primeCarouselMode) {
                    // Prime Video style: الحركة تلحق الهدف باستمرار في الأسفل.
                    tCarousel = 1.0f;
                    tZoom = 1.0f;
                    tSelection = 1.0f;
                } else {
                    // الوضع الأصلي لبقية البلوجينات والقوائم والـ grid.
                    if (w.animStartTime < 0.0f) w.animStartTime = currentTime;
                    float elapsed = currentTime - w.animStartTime;
                    if (elapsed < 0.0f) elapsed = 0.0f;
                    if (w.carouselDuration > 0.0f) {
                        tCarousel = fmin(1.0f, elapsed / w.carouselDuration);
                        float invT = 1.0f - tCarousel;
                        tCarousel = 1.0f - (invT * invT * invT * invT);
                    }
                    if (w.selectionDuration > 0.0f) {
                        tSelection = fmin(1.0f, elapsed / w.selectionDuration);
                        float invS = 1.0f - tSelection;
                        tSelection = 1.0f - (invS * invS * invS * invS);
                    } else {
                        tSelection = tCarousel;
                    }
                    if (w.zoomEnabled && w.zoomDuration > 0.0f) {
                        tZoom = fmin(1.0f, elapsed / w.zoomDuration);
                        float invZ = 1.0f - tZoom;
                        tZoom = 1.0f - (invZ * invZ * invZ * invZ);
                    }
                    bool carouselDone = (tCarousel >= 1.0f);
                    bool zoomDone = (!w.zoomEnabled || tZoom >= 1.0f);
                    bool selectionDone = (tSelection >= 1.0f);
                    if (carouselDone && zoomDone && selectionDone) {
                        w.isAnimating = false;
                        anyWidgetAnimating.store(false);
                        manage_widget_textures(w);
                        tCarousel = 1.0f;
                    tZoom = 1.0f;
                    tSelection = 1.0f;
                    }
                }
            }

            // 🔍 تقدّم انتقال التكبير — لا يدخل هنا إلا أثناء الانتقال فعلياً
            if (w.zoomShiftStart >= 0.0f || w.zoomShift != w.zoomShiftTo) {
                if (w.zoomShiftStart < 0.0f) w.zoomShiftStart = currentTime;
                float dur = (w.zoomShiftDur > 0.01f) ? w.zoomShiftDur : 0.15f;
                float t   = (currentTime - w.zoomShiftStart) / dur;
                if (t >= 1.0f) {
                    w.zoomShift      = w.zoomShiftTo;
                    w.zoomShiftStart = -1.0f;
                } else {
                    if (t < 0.0f) t = 0.0f;
                    float e = 1.0f - (1.0f - t) * (1.0f - t);   // easeOutQuad: هادئ بلا ذيل طويل
                    w.zoomShift = w.zoomShiftFrom + (w.zoomShiftTo - w.zoomShiftFrom) * e;
                    force_render = true;
                }
            }
            const float effZoomFactor = (w.zoomShift >= 0.9995f)
                                        ? w.zoomFactor
                                        : (1.0f + (w.zoomFactor - 1.0f) * w.zoomShift);

            float gap = w.itemGap;
            // حساب التمرير بناءً على اتجاه الـ Widget (أفقي أو عمودي)
            float totalSize = 0.0f;
            if (w.orientId == OrientId::Grid) {
                int totalRows = (w.items.size() + w.gridColumns - 1) / w.gridColumns;
                totalSize = totalRows * w.itemH + (totalRows > 0 ? (totalRows - 1) * gap : 0);
            } else if (w.orientId == OrientId::Vertical) {
                totalSize = w.items.size() * w.itemH + (w.items.size() > 0 ? (w.items.size() - 1) * gap : 0);
            } else {
                totalSize = w.items.size() * w.itemW + (w.items.size() > 0 ? (w.items.size() - 1) * gap : 0);
            }

            // 🚀 إصلاح مقاسات الكاميرا: إجبار المحرك على معاملة الـ Grid كقائمة عمودية
            bool isVert = (w.orientId == OrientId::Vertical || w.orientId == OrientId::Grid);
            float zoomExtra = w.zoomEnabled ? ((w.zoomFactor - 1.0f) * (isVert ? w.itemH : w.itemW)) : 0.0f;
            float viewSize = isVert ? w.h : w.w;
            float offsetBase = isVert ? w.itemOffsetY : w.itemOffsetX;
            
            float maxScroll = offsetBase + totalSize - viewSize + (zoomExtra / 2.0f) + 10.0f;
            if (maxScroll < 0.0f) maxScroll = 0.0f; 
            
            float rawTargetOffset = 0.0f;
            if (w.orientId == OrientId::Grid) {
                // 🚀 خوارزمية القفز بالصفوف (Discrete Step) - الحل النهائي لمشكلة الـ gap
                int currentRow = w.selectedIndex / w.gridColumns;
                float itemStep = w.itemH + gap; // يحسب العنصر + الفراغ بدقة متناهية
                
                // 🚀 إضافة 2.0f لتفادي أخطاء الفواصل العشرية (Float Precision) التي تسبب الارتداد
                int topVisibleRow = (int)((w.currentScrollOffset + 2.0f) / itemStep);
                
                // نحن نريد عرض صفين فقط دائماً كما حددت
                int visibleRows = 2;
                
                if (currentRow >= topVisibleRow + visibleRows) {
                    // إذا نزلنا تحت الصفين، ادفع الكاميرا لأسفل بمقدار صف واحد كامل
                    rawTargetOffset = (currentRow - visibleRows + 1) * itemStep;
                } 
                else if (currentRow < topVisibleRow) {
                    // إذا صعدنا للأعلى، اسحب الكاميرا لأعلى بمقدار صف كامل
                    rawTargetOffset = currentRow * itemStep;
                } 
                else {
                    // إذا كنا نتحرك داخل الصفين المرئيين، لا تحرك الكاميرا إطلاقاً
                    rawTargetOffset = topVisibleRow * itemStep;
                }

                if (rawTargetOffset < 0.0f) rawTargetOffset = 0.0f;
            } else {
                float itemStep = ((w.orientId == OrientId::Vertical) ? w.itemH : w.itemW) + gap;
                if (w.orientId == OrientId::Vertical) {
                    // 🚀 للقوائم العمودية: إرجاع حساب التوسيط ليتوقف الإطار في المنتصف وتنزلق القائمة تحته
                    float centerOffset = (viewSize / 2.0f) - (itemStep / 2.0f);
                    rawTargetOffset = (w.selectedIndex * itemStep) - centerOffset;
                } else {
                    // 🚀 للقوائم الأفقية: تثبيت الإطار في العنصر الأول (اليسار) كما فعلنا سابقاً
                    rawTargetOffset = (w.selectedIndex * itemStep);
                }
                
                if (rawTargetOffset < 0.0f) rawTargetOffset = 0.0f;
            }
            
            float targetOffset = rawTargetOffset;
            if (w.carouselLimit == 0) { 
                // 0 = التوقف عند النهاية (الوضع الطبيعي للحفاظ على القوائم العادية)
                if (maxScroll <= 0.0f) {
                    // لا يوجد Scroll أصلاً (مثلاً category_list)
                    targetOffset = 0.0f;
                } else {
                    targetOffset = std::max(0.0f, std::min(rawTargetOffset, maxScroll));
                }
            }

            if (w.isAnimating) {
                if (primeCarouselMode) {
                    // 🚀 Prime-style continuous target scrolling لكاروسال البوسترات فقط.
                    float followSpeed = 20.0f;
                    if (w.carouselDuration > 0.0f) {
                        followSpeed = std::max(10.0f, 8.0f / w.carouselDuration);
                    }
                    float follow = 1.0f - expf(-followSpeed * frameDt);
                    w.currentScrollOffset += (targetOffset - w.currentScrollOffset) * follow;
                    w.visualIndex += ((float)w.selectedIndex - w.visualIndex) * follow;

                    float zoomFollowSpeed = 20.0f;
                    if (w.zoomDuration > 0.0f) zoomFollowSpeed = std::max(10.0f, 8.0f / w.zoomDuration);
                    float zoomFollow = 1.0f - expf(-zoomFollowSpeed * frameDt);
                    w.zoomVisualIndex += ((float)w.selectedIndex - w.zoomVisualIndex) * zoomFollow;

                    float offsetErr = fabs(targetOffset - w.currentScrollOffset);
                    float indexErr = fabs((float)w.selectedIndex - w.visualIndex);
                    if (offsetErr < 0.35f && indexErr < 0.01f) {
                        w.currentScrollOffset = targetOffset;
                        w.startScrollOffset = targetOffset;
                        w.visualIndex = (float)w.selectedIndex; w.zoomVisualIndex = (float)w.selectedIndex;;
                        w.isAnimating = false;
                        anyWidgetAnimating.store(false);
                        manage_widget_textures(w);
                    }
                } else {
                    // الوضع الأصلي لبقية القوائم والـ grid.
                    w.currentScrollOffset = w.startScrollOffset + (targetOffset - w.startScrollOffset) * tCarousel;
                    // منع اهتزاز آخر فريم بسبب float precision
                    if (fabs(w.currentScrollOffset - targetOffset) < 0.01f)
                        w.currentScrollOffset = targetOffset;
                }
            } else {
                w.currentScrollOffset = targetOffset;
                w.startScrollOffset = targetOffset;
                w.visualIndex = (float)w.selectedIndex; w.zoomVisualIndex = (float)w.selectedIndex;;
            }

            // ================= NEW: WIDGET VERTICAL AUTO-SCROLL (BLOCK SCROLL) =================
            float autoScrollY = 0.0f;
            if (w.scrollTextMode == 2 && totalSize > viewSize && (w.orientId == OrientId::Vertical || w.orientId == OrientId::Grid)) {
                if (w.textScrollActive) {
                    if (w.textScrollStartTime < 0.0f) w.textScrollStartTime = currentTime;
                    float elapsed = currentTime - w.textScrollStartTime;
                    float delay = w.scrollDelay >= 0.0f ? w.scrollDelay : 2.0f;
                    if (elapsed > delay) {
                        float activeTime = elapsed - delay;
                        float speed = 60.0f; // سرعة ناعمة للقراءة
                        float L_y = totalSize + viewSize; // إجمالي مسافة الرحلة (صعود ثم ظهور من الأسفل)
                        float D_y = fmod(activeTime * speed, L_y);
                        int loops = (int)((activeTime * speed) / L_y);
                        
                        if (loops >= 1) { 
                            w.textScrollActive = false; // التوقف بعد دورة واحدة كاملة
                            autoScrollY = 0.0f;
                        } else {
                            if (D_y <= totalSize) {
                                autoScrollY = D_y; // الصعود للأعلى
                            } else {
                                autoScrollY = D_y - L_y; // الانتقال للأسفل للظهور مجدداً
                            }
                        }
                    }
                }
            }
            // ====================================================================

            // ✅ استخراج مسافة التوهج والظل (Padding) لمنع القص
            float clipPadding = 10.0f;
            if (w.selectionPadding >= 0.0f) clipPadding = w.selectionPadding;
            else clipPadding = (w.itemShape == ItemShapeId::Circle) ? 4.0f : 10.0f; 

            // 🚀 توسيع منطقة القص (Scissor Test) للسماح لتأثير الزووم والتدرج اللوني بصورة التحديد بالظهور كاملاً دون أي قطع
            float clipPadX = (w.zoomEnabled ? (w.itemW * (w.zoomFactor - 1.0f) / 2.0f) : 0.0f) + clipPadding + 5.0f;
            float clipPadY = (w.zoomEnabled ? (w.itemH * (w.zoomFactor - 1.0f) / 2.0f) : 0.0f) + clipPadding + 5.0f;

            // 🚀 توسيع منطقة القص للأسفل حتى لا يختفي الانعكاس
            if (w.hasReflection) {
                clipPadY += (w.itemH * 0.17f); // ✅ 2% مسافة + 15% انعكاس = 17%
            }
            
            glEnable(GL_SCISSOR_TEST);
            // 🚀 تعديل منطقة القص لتسمح بنزول النص لأسفل الـ Widget
            // ✅ إصلاح: الشبكة (grid) هي حاوية حقيقية لها حدود، فلا يجوز أن يتسرّب
            //    الصف التالي خارجها. لذلك نُلغي المساحة الإضافية السفلية للشبكات
            //    ونقصّها تماماً عند حافة الويدجت السفلى (Clipping Mask حقيقي).
            //    القوائم الأفقية/العمودية تحتفظ بسلوكها القديم (نصوص وانعكاسات تحتها).
            bool clipTightBottom = (w.orientId == OrientId::Grid && !w.hasReflection);
            int   extraSpaceForText = clipTightBottom ? 0 : 150; // أضف 150 بكسل إضافية للأسفل
            float clipPadBottom     = clipTightBottom ? 0.0f : clipPadY;
            glScissor((int)(w.x + elemOffsetX - clipPadX), 
                    h - (int)(w.y + elemOffsetY + w.h + clipPadBottom + extraSpaceForText), 
                    (int)(w.w + clipPadX * 2.0f), 
                    (int)(w.h + clipPadY + clipPadBottom + extraSpaceForText));

            gl_set_blend(false);
            
            // 🚀 حركة الويدجت لا تعني فقط w.isAnimating؛ أحياناً Grid كامل يتحرك بأنيميشن شاشة/عنصر.
            bool widgetMotionActive = w.isAnimating || currentScreen.slideActive || w.localAnimActive;

            // 🚀 السحر الحقيقي لإنهاء تشنج الواجهة (Smart Culling) 🚀
            size_t loopCount = w.items.size();
            bool isSkeletonLoading = (loopCount > 0 && w.items[0].imagePath == "loading_state");
            
            size_t startIdx = 0;
            size_t endIdx = loopCount;

            if (isSkeletonLoading) {
                endIdx = 15;
            } else if (loopCount > 20) {
                // حساب العناصر الظاهرة فقط بدلاً من آلاف الأفلام!
                float gap = w.itemGap;
                float step = (w.orientId == OrientId::Vertical || w.orientId == OrientId::Grid) ? w.itemH + gap : w.itemW + gap;
                
                int visualIndex = 0;
                if (step > 0.0f) {
                    visualIndex = (int)((w.currentScrollOffset + autoScrollY) / step);
                }
                
                if (w.orientId == OrientId::Grid) {
                    visualIndex *= w.gridColumns;
                }
                
                int itemsOnScreen = (w.orientId == OrientId::Grid) ? (w.gridColumns * 3) : 12;
                int buffer = (w.orientId == OrientId::Grid) ? (w.gridColumns * 1) : 8;

                // 🚀 نفس إصلاح النصوص: عدد العناصر المرسومة يتبع مقاس الويدجت الحقيقي
                // حتى لا تُقصّ العناصر السفلية في القوائم الطويلة (أكثر من 12 عنصراً مرئياً).
                if (step > 1.0f) {
                    float visibleExtent = (w.orientId == OrientId::Horizontal) ? w.w : w.h;
                    int visibleOnScreen = (int)ceil(visibleExtent / step) + 1;
                    if (w.orientId == OrientId::Grid) {
                        visibleOnScreen *= std::max(1, w.gridColumns);
                        if (visibleOnScreen > itemsOnScreen) {
                            itemsOnScreen = visibleOnScreen;
                            buffer = std::max(buffer, w.gridColumns);   // صف حماية واحد
                        }
                    } else if (visibleOnScreen > itemsOnScreen) {
                        itemsOnScreen = visibleOnScreen;
                        buffer        = std::max(buffer, visibleOnScreen / 2);
                    }
                }
                // 🚀 Grid Motion Lite: أثناء حركة Grid لا نرسم 10 صفوف، فقط المرئي + صف حماية.
                // هذا يحسن حركة carousel داخل Grid وحركة slide يمين/يسار بدون إخفاء النصوص المرئية.
                if (widgetMotionActive && w.orientId == OrientId::Grid) {
                    float stepY = w.itemH + gap;
                    int visibleRows = (stepY > 1.0f) ? ((int)ceil(w.h / stepY) + 1) : 3;
                    itemsOnScreen = w.gridColumns * std::max(2, visibleRows);
                    buffer = w.gridColumns * 1;
                }
                // 🚀 استثناء لقائمة الحروف لكي لا يقوم المحرك بإخفائها
                if (w.name == "az_list") { itemsOnScreen = std::max(itemsOnScreen, 30); }
                
                int minI = visualIndex - buffer;
                int maxI = visualIndex + itemsOnScreen + buffer;
                
                // حماية العنصر المحدد لضمان بقائه في الذاكرة أثناء التنقل السريع
                int selMin = w.selectedIndex - buffer;
                int selMax = w.selectedIndex + itemsOnScreen + buffer;
                
                minI = std::min(minI, selMin);
                maxI = std::max(maxI, selMax);
                
                startIdx = (minI < 0) ? 0 : minI;
                endIdx = (maxI >= (int)loopCount) ? loopCount : maxI + 1;
            }

            // 🚀 Motion Lite للكاروسال: أثناء حركة البوسترات نرسم فقط العناصر القريبة جداً من
            // selected/prev بدل 20+ عنصر. هذا أهم علاج لثقل Carousel البوسترات.
            bool carouselLiteMode = (primeCarouselMode && w.isAnimating && !isSkeletonLoading && loopCount > 0);
            bool gridMotionLiteMode = (w.orientId == OrientId::Grid && widgetMotionActive && !isSkeletonLoading && loopCount > 0 && w.name != "az_list");
            bool posterMotionLiteMode = (carouselLiteMode || gridMotionLiteMode);
            if (carouselLiteMode && w.name != "az_list") {
                int visualCenter = (int)floor(w.visualIndex + 0.5f);
                int centerMin = std::min(std::min(w.prevIndex, w.selectedIndex), visualCenter);
                int centerMax = std::max(std::max(w.prevIndex, w.selectedIndex), visualCenter);
                int motionPad = 4;
                if (w.orientId == OrientId::Horizontal) {
                    float step = w.itemW + gap;
                    int visible = (step > 1.0f) ? (int)ceil(w.w / step) + 2 : 6;
                    motionPad = std::max(3, visible / 2 + 2);
                } else if (w.orientId == OrientId::Vertical) {
                    float step = w.itemH + gap;
                    int visible = (step > 1.0f) ? (int)ceil(w.h / step) + 2 : 6;
                    motionPad = std::max(3, visible / 2 + 2);
                } else if (w.orientId == OrientId::Grid) {
                    motionPad = std::max(w.gridColumns * 2, 4);
                }
                size_t mStart = (centerMin - motionPad < 0) ? 0 : (size_t)(centerMin - motionPad);
                size_t mEnd = (centerMax + motionPad >= (int)loopCount) ? loopCount : (size_t)(centerMax + motionPad + 1);
                startIdx = std::max(startIdx, mStart);
                endIdx = std::min(endIdx, mEnd);
                if (startIdx >= endIdx) { startIdx = mStart; endIdx = mEnd; }
            }

            // =========================================================
            // 🚀 1. رسم خلفيات العناصر (itemcolor) أولاً لتكون تحت التحديد
            // =========================================================
            auto perfBgStart = std::chrono::high_resolution_clock::now();
            for (size_t i = startIdx; i < endIdx; i++) {
                bool isDummy = (i >= w.items.size());
                
                if (!isDummy && w.items[i].poolIndex != -1) {
                    int pIdx = w.items[i].poolIndex;
                    if (!texturePool[pIdx].isLoading && !texturePool[pIdx].dataReady) {
                        w.items[i].loaded = true;
                        if (texturePool[pIdx].width > 0) {
                            w.items[i].textureId = texturePool[pIdx].textureId;
                            const int dc = texturePool[pIdx].domColor.load();   // 🎨
                            if (dc >= 0) {
                                w.items[i].domR = ((dc >> 16) & 0xFF) / 255.0f;
                                w.items[i].domG = ((dc >>  8) & 0xFF) / 255.0f;
                                w.items[i].domB = ( dc        & 0xFF) / 255.0f;
                                w.items[i].domValid = true;
                            }
                        }
                        else w.items[i].textureId = 0;
                    }
                }

                float currentItemW = (!isDummy && w.items[i].customW > 0.0f) ? w.items[i].customW : w.itemW;
                float currentItemH = (!isDummy && w.items[i].customH > 0.0f) ? w.items[i].customH : w.itemH;
                float itemX = 0.0f, itemY = 0.0f;   // تهيئة صريحة: بعض فروع الاتجاه كانت تتركهما بلا قيمة
                if (!isDummy && w.items[i].customX >= 0.0f && w.items[i].customY >= 0.0f) {
                    itemX = w.x + elemOffsetX + w.items[i].customX;
                    itemY = w.y + elemOffsetY + w.items[i].customY - autoScrollY;
                } else {
                    if (w.orientId == OrientId::Vertical) {
                        itemX = w.x + elemOffsetX + (w.w - currentItemW) / 2.0f + w.itemOffsetX; 
                        float step = w.itemH + gap; 
                        itemY = w.y + elemOffsetY + w.itemOffsetY + (i * step) - w.currentScrollOffset - autoScrollY;
                    } else if (w.orientId == OrientId::Horizontal) {
                        itemX = w.x + elemOffsetX + w.itemOffsetX + (i * (w.itemW + gap)) - w.currentScrollOffset; 
                        itemX += (w.itemW - currentItemW) / 2.0f; 
                        //itemY = w.y + elemOffsetY + (w.h - w.itemH) / 2.0f + w.itemOffsetY - autoScrollY;
                        itemY = w.y + elemOffsetY + floor((w.h - w.itemH) / 2.0f) + w.itemOffsetY - autoScrollY; 
                    } else if (w.orientId == OrientId::Grid) {
                        int col = i % w.gridColumns; int row = i / w.gridColumns;
                        itemX = w.x + elemOffsetX + w.itemOffsetX + (col * (w.itemW + gap));
                        itemY = w.y + elemOffsetY + w.itemOffsetY + (row * (w.itemH + gap)) - w.currentScrollOffset - autoScrollY;
                    }
                }

                float scale = 1.0f;
                if (w.zoomEnabled && !isDummy) {
                    if (primeCarouselMode) {
                        float focusIndex = w.isAnimating ? w.zoomVisualIndex : (float)w.selectedIndex;
                        float dist = fabs((float)i - focusIndex);
                        float focus = 1.0f - std::min(1.0f, dist);
                        scale = 1.0f + (effZoomFactor - 1.0f) * focus;
                    } else {
                        if ((int)i == w.selectedIndex) {
                            if (w.isAnimating) scale = 1.0f + (effZoomFactor - 1.0f) * tZoom;
                            else scale = effZoomFactor;
                        } else if ((int)i == w.prevIndex && w.isAnimating) {
                            scale = effZoomFactor - (effZoomFactor - 1.0f) * tZoom;
                        }
                    }
                }

                float drawW = currentItemW * scale; float drawH = currentItemH * scale;
                float drawX = itemX - (drawW - currentItemW) / 2.0f; float drawY = itemY - (drawH - currentItemH) / 2.0f;
                
                // if (!w.isAnimating && !currentScreen.slideActive) {
                //     drawX = floor(drawX - elemOffsetX + 0.5f) + elemOffsetX; drawY = floor(drawY - elemOffsetY + 0.5f) + elemOffsetY;
                //     drawW = floor(drawW + 0.5f); drawH = floor(drawH + 0.5f);
                // }

                // 🚀 هامش القص: كان 1000px فيُرسم ما يقارب خمسة صفوف خارج الشاشة
                //    فوق وتحت (٧٠ عنصراً بدل ١٥) وهو أثقل شيء في الشبكات النصية.
                g_perf_items.fetch_add(1);
                if (drawX + drawW < w.x + elemOffsetX - 20.0f || drawX > w.x + elemOffsetX + w.w + 20.0f ||
                    drawY + drawH < w.y + elemOffsetY - 20.0f || drawY > w.y + elemOffsetY + w.h + 20.0f) {
                    continue; 
                }
                g_perf_drawn.fetch_add(1);
                
                float currentItemRadius = w.itemCornerRadius;
                if (w.itemShape == ItemShapeId::Circle) currentItemRadius = std::min(drawW, drawH) / 2.0f;

                bool drawItemColor = (w.useItemColor && w.itemBgA > 0.01f);
                if (!isDummy && w.items[i].loaded && w.items[i].textureId != 0) { drawItemColor = false; }
                // 🚀 مستطيل بلا مساحة فعلية (شريط تقدّم فارغ مثلاً) لا يستحق نداء رسم
                if (drawW < 2.0f || drawH < 2.0f) { drawItemColor = false; }
                
                if (drawItemColor) {
                    float iv[] = { drawX, drawY, drawX + drawW, drawY, drawX, drawY + drawH, drawX + drawW, drawY + drawH };
                    if (currentItemRadius > 0.0f || w.itemBgA * elemAlpha < 1.0f) gl_set_blend(true); else gl_set_blend(false);
                    
                    // 🚀 تفعيل التدرج اللوني لخلفية العنصر
                    glUniform1f(gradientModeLoc, (float)w.itemBgGradientMode);
                    if (w.itemBgGradientMode > 0) {
                        glUniform4f(colorEndLoc, w.itemBgR2, w.itemBgG2, w.itemBgB2, w.itemBgA2 * elemAlpha);
                    }
                    
                    glUniform1f(useTexLoc, 0.0f); 
                    glUniform4f(colorLoc, w.itemBgR, w.itemBgG, w.itemBgB, w.itemBgA * elemAlpha);
                    glUniform1f(roundLoc, currentItemRadius);
                    glUniform2f(rectPosLoc, drawX, drawY);
                    glUniform2f(boxSizeLoc, drawW, drawH);
                    glUniform1f(borderWidthLoc, 0.0f);
                    
                    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, iv);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    
                    glUniform1f(gradientModeLoc, 0.0f); // 🚀 تصفير התدرج فوراً لمنع تأثيره على العناصر اللاحقة
                }
            }

            g_perf_bg_us.fetch_add((int)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - perfBgStart).count());

            // =========================================================
            // 🚀 رسم إطار التحديد أولاً (كخلفية للنص المختار)
            // =========================================================
            float selX = 0.0f, selY = 0.0f, selW = 0.0f, selH = 0.0f, pad = 0.0f, currentSelRadius = 0.0f;
            if (w.showSelection && w.itemW > 0 && w.itemH > 0 && w.items.size() > 0) {
                // ✅ حساب الإحداثي (الفهرس) المنزلق إذا كان الأنيميشن مفعلاً
                float currentAnimatedIndex = w.selectedIndex;
                float currentSelW = (w.selectionW > 0.0f) ? w.selectionW : ((w.items[w.selectedIndex].customW > 0.0f) ? w.items[w.selectedIndex].customW : w.itemW);
                float currentSelH = (w.selectionH > 0.0f) ? w.selectionH : ((w.items[w.selectedIndex].customH > 0.0f) ? w.items[w.selectedIndex].customH : w.itemH);

                float selBaseX = 0.0f, selBaseY = 0.0f;   // تهيئة صريحة
                if (w.items[w.selectedIndex].customX >= 0.0f && w.items[w.selectedIndex].customY >= 0.0f) {
                    float targetX = w.x + elemOffsetX + w.items[w.selectedIndex].customX;
                    float targetY = w.y + elemOffsetY + w.items[w.selectedIndex].customY;

                    if (w.selectionpixmapAnim && w.isAnimating) {
                        float prevX = w.x + elemOffsetX + w.items[w.prevIndex].customX;
                        float prevY = w.y + elemOffsetY + w.items[w.prevIndex].customY;
                        selBaseX = prevX + (targetX - prevX) * tSelection;
                        selBaseY = prevY + (targetY - prevY) * tSelection;

                        float prevSelW = (w.selectionW > 0.0f) ? w.selectionW : ((w.items[w.prevIndex].customW > 0.0f) ? w.items[w.prevIndex].customW : w.itemW);
                        float prevSelH = (w.selectionH > 0.0f) ? w.selectionH : ((w.items[w.prevIndex].customH > 0.0f) ? w.items[w.prevIndex].customH : w.itemH);
                        currentSelW = prevSelW + (currentSelW - prevSelW) * tSelection;
                        currentSelH = prevSelH + (currentSelH - prevSelH) * tSelection;
                    } else {
                        selBaseX = targetX;
                        selBaseY = targetY;
                    }
                } else {
                    if (w.selectionpixmapAnim && w.isAnimating) {
                        currentAnimatedIndex = primeCarouselMode ? w.visualIndex : (w.prevIndex + (w.selectedIndex - w.prevIndex) * tSelection);
                        float prevSelW = (w.selectionW > 0.0f) ? w.selectionW : ((w.items[w.prevIndex].customW > 0.0f) ? w.items[w.prevIndex].customW : w.itemW);
                        currentSelW = prevSelW + (currentSelW - prevSelW) * tSelection;
                        float prevSelH = (w.selectionH > 0.0f) ? w.selectionH : ((w.items[w.prevIndex].customH > 0.0f) ? w.items[w.prevIndex].customH : w.itemH);
                        currentSelH = prevSelH + (currentSelH - prevSelH) * tSelection;
                    }

                    // 🚀 السحر هنا: استخدام الهدف النهائي للتمرير إذا كانت حركة الإطار معطلة، ليبقى الإطار ثابتاً وتنزلق البوسترات تحته
                    float offsetToUse = (w.selectionpixmapAnim && w.isAnimating) ? w.currentScrollOffset : targetOffset;
                    if (w.orientId == OrientId::Vertical) {
                        selBaseX = w.x + elemOffsetX + (w.w - currentSelW) / 2.0f + w.itemOffsetX;
                        selBaseY = w.y + elemOffsetY + w.itemOffsetY + (currentAnimatedIndex * (w.itemH + gap)) - offsetToUse;
                    } else if (w.orientId == OrientId::Horizontal) {
                        selBaseX = w.x + elemOffsetX + w.itemOffsetX + (currentAnimatedIndex * (w.itemW + gap)) - offsetToUse;
                        selBaseX += (w.itemW - currentSelW) / 2.0f;
                        selBaseY = w.y + elemOffsetY + (w.h - w.itemH) / 2.0f + w.itemOffsetY;
                    } else if (w.orientId == OrientId::Grid) { // ✅ خوارزمية الانزلاق الذكي ثنائي الأبعاد (2D Glide)
                        
                        // 1. استرجاع نقطة البداية والنهاية مباشرة من الذاكرة المستقلة للويدجت (بدون تداخل)
                        int oldIdx = w.prevIndex;
                        int newIdx = w.selectedIndex;
                        
                        // 2. حساب نسبة تقدم الأنيميشن (من 0.0 إلى 1.0)
                        float t = 1.0f;
                        if (newIdx != oldIdx) {
                            t = 1.0f - std::abs((currentAnimatedIndex - newIdx) / (float)(newIdx - oldIdx));
                            if (t < 0.0f) t = 0.0f;
                            if (t > 1.0f) t = 1.0f;
                        }
                        
                        // 3. تحديد الأعمدة والصفوف بدقة عشرية
                        float startCol = oldIdx % w.gridColumns;
                        float startRow = oldIdx / w.gridColumns;
                        float endCol = newIdx % w.gridColumns;
                        float endRow = newIdx / w.gridColumns;

                        float currentCol = startCol + (endCol - startCol) * t;
                        float currentRow = startRow + (endRow - startRow) * t;
                        
                        // 4. تطبيق الإحداثيات على الشاشة
                        selBaseX = w.x + elemOffsetX + w.itemOffsetX + (currentCol * (w.itemW + gap));
                        selBaseY = w.y + elemOffsetY + w.itemOffsetY + (currentRow * (w.itemH + gap)) - offsetToUse;
                    }
                }

                // 🚀 السحر هنا: تثبيت مقاس الإطار على معامل الزووم النهائي (w.zoomFactor)
                // ليبقى الإطار كبيراً وثابتاً ولا يتأثر بمرحلة التمدد التدريجي (Animate)
                float finalZoomScale = w.zoomEnabled ? effZoomFactor : 1.0f;
                
                // 🚀 تطبيق الزووم النهائي على أبعاد الإطار
                selW = currentSelW * finalZoomScale;
                selH = currentSelH * finalZoomScale;
                
                // 🚀 تطبيق الزووم النهائي على موضعه ليبقى متمركزاً تماماً
                selX = selBaseX - (selW - currentSelW) / 2.0f + w.selectionOffsetX;
                selY = selBaseY - (selH - currentSelH) / 2.0f + w.selectionOffsetY;

                // // 🚀 تحرير الحركة: التقريب يحدث في حالة السكون فقط للحصول على نعومة مطلقة
                // if (!w.isAnimating && !currentScreen.slideActive) {
                //     selX = floor(selX + 0.5f);
                //     selY = floor(selY + 0.5f);
                //     selW = floor(selW + 0.5f);
                //     selH = floor(selH + 0.5f);
                // }

                // ✅ نظام الـ Padding الذكي
                pad = 10.0f;
                if (w.selectionPadding >= 0.0f) {
                    pad = w.selectionPadding;
                } else {
                    pad = (w.itemShape == ItemShapeId::Circle) ? 4.0f : 10.0f; 
                }
                
                currentSelRadius = w.selectionCornerRadius;
                if (w.itemShape == ItemShapeId::Circle) {
                    currentSelRadius = std::min(selW + (pad * 2.0f), selH + (pad * 2.0f)) / 2.0f;
                }

                // ==========================================================================
                // ✨ الهالة: أربع طبقات مستديرة متحدة المركز تتسع وتخفت.
                //    تُرسم قبل البوسترات فتبدو نوراً يتسرّب من خلف العنصر المحدد،
                //    وتُحجب تماماً أثناء الحركة ⇒ تكلفة صفر وقت الكاروسال، وتظهر
                //    في **نفس الإطار** الذي يستقرّ فيه الفوكس (بلا تلاشٍ متأخر).
                // ==========================================================================
                if (w.glowEnabled && w.glowOpacity > 0.003f && elemAlpha > 0.01f &&
                    !w.isAnimating && !currentScreen.slideActive) {
                    float gR = 1.0f, gG = 1.0f, gB = 1.0f;
                    if (w.glowHasColor)          { gR = w.glowR;      gG = w.glowG;      gB = w.glowB; }
                    else if (w.selectionIsBorder){ gR = w.selBorderR; gG = w.selBorderG; gB = w.selBorderB; }
                    else if (w.selectionIsColor) { gR = w.selR;       gG = w.selG;       gB = w.selB; }

                    const int   GLOW_LAYERS = 4;
                    const float GLOW_SPREAD = (w.glowSize > 0.0f) ? w.glowSize : 30.0f;

                    gl_set_blend(true);
                    glUniform1f(useTexLoc, 0.0f);
                    glUniform1f(gradientModeLoc, 0.0f);
                    glUniform1f(borderWidthLoc, 0.0f);

                    for (int gi = GLOW_LAYERS - 1; gi >= 0; --gi) {
                        float grow = GLOW_SPREAD * ((float)(gi + 1) / (float)GLOW_LAYERS);
                        float fall = 1.0f - ((float)gi / (float)GLOW_LAYERS);
                        float ga   = w.glowOpacity * fall * fall * (1.7f / (float)GLOW_LAYERS);
                        if (ga < 0.002f) continue;

                        float gx = selX - pad - grow;
                        float gy = selY - pad - grow;
                        float gw = selW + (pad * 2.0f) + (grow * 2.0f);
                        float gh = selH + (pad * 2.0f) + (grow * 2.0f);

                        glUniform4f(colorLoc, gR, gG, gB, ga * elemAlpha);
                        glUniform1f(roundLoc, currentSelRadius + grow);
                        glUniform2f(rectPosLoc, gx, gy);
                        glUniform2f(boxSizeLoc, gw, gh);
                        float gv[] = { gx, gy, gx + gw, gy, gx, gy + gh, gx + gw, gy + gh };
                        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, gv);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    }
                }

                if (w.selectionIsColor) {
                    // ✅ إصلاح الهالة/الخط الداكن حول الفوكس:
                    // الشادر يُخرج ألواناً مضروبة مسبقاً بألفا التنعيم (finalColor.rgb *= a)،
                    // فإذا أُطفئ الـ Blend كُتبت بكسلات الحافة المنعّمة كلون داكن فوق الخلفية
                    // بدل مزجها معها ⇒ إطار أسود رفيع حول الزوايا الدائرية.
                    // اللون المصمت مع GL_ONE/GL_ONE_MINUS_SRC_ALPHA يعطي نفس النتيجة داخلياً،
                    // لذلك نُبقي الـ Blend مفعّلاً دائماً هنا.
                    gl_set_blend(true);
                    glUniform1f(useTexLoc, 0.0f);
                    glUniform4f(colorLoc, w.selR, w.selG, w.selB, w.selA * elemAlpha);
                    
                    glUniform1f(gradientModeLoc, (float)w.selGradientMode);
                    if (w.selGradientMode > 0) {
                        glUniform4f(colorEndLoc, w.selR2, w.selG2, w.selB2, w.selA2 * elemAlpha);
                    }
                    
                    glUniform1f(roundLoc, currentSelRadius);
                    glUniform1f(borderWidthLoc, 0.0f); // 🚀 حماية إضافية لمنع الإطارات السابقة من التأثير
                    glUniform2f(rectPosLoc, selX - pad, selY - pad);
                    glUniform2f(boxSizeLoc, selW + (pad * 2.0f), selH + (pad * 2.0f));
                    float iv[] = { selX - pad, selY - pad, selX + selW + pad, selY - pad, selX - pad, selY + selH + pad, selX + selW + pad, selY + selH + pad };
                    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, iv);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glUniform1f(gradientModeLoc, 0.0f); // ✅ تصفير التدرج فوراً للحماية
                    gl_set_blend(true);
                }
            }
            // 🚀 الآن الحلقة ستدور 20 مرة كحد أقصى بدلاً من 15000 مرة!
            auto perfTxtStart = std::chrono::high_resolution_clock::now();
            for (size_t i = startIdx; i < endIdx; i++) {
                bool isDummy = (i >= w.items.size()); // هل هذا الصندوق وهمي أم حقيقي؟
                
                if (!isDummy && w.items[i].poolIndex != -1) {
                    int pIdx = w.items[i].poolIndex;
                    // 🚀 إذا انتهت محاولة التحميل (سواء بنجاح أو فشل)
                    if (!texturePool[pIdx].isLoading && !texturePool[pIdx].dataReady) {
                        w.items[i].loaded = true; // نعتبره مكتمل لكي يختفي السپينر!
                        if (texturePool[pIdx].width > 0) {
                            w.items[i].textureId = texturePool[pIdx].textureId;
                            const int dc = texturePool[pIdx].domColor.load();   // 🎨
                            if (dc >= 0) {
                                w.items[i].domR = ((dc >> 16) & 0xFF) / 255.0f;
                                w.items[i].domG = ((dc >>  8) & 0xFF) / 255.0f;
                                w.items[i].domB = ( dc        & 0xFF) / 255.0f;
                                w.items[i].domValid = true;
                            }
                        } else {
                            w.items[i].textureId = 0; // الرابط ميت، نعطيه 0 لكي لا يعرض مربعاً أسود
                        }
                    }
                }

                // 🚀 ربط Texture ID الخاص بالأفاتار متى ما أصبح جاهزاً
                if (!isDummy && w.items[i].avatarPoolIndex != -1) {
                    int apIdx = w.items[i].avatarPoolIndex;
                    if (!texturePool[apIdx].isLoading && !texturePool[apIdx].dataReady) {
                        w.items[i].avatarLoaded = true;
                        if (texturePool[apIdx].width > 0) w.items[i].avatarTexId = texturePool[apIdx].textureId;
                        else w.items[i].avatarTexId = 0;
                    }
                }

                // ✅ 1. الحسابات الهندسية (المربعات الوهمية تأخذ الحجم الافتراضي دائماً)
                float currentItemW = (!isDummy && w.items[i].customW > 0.0f) ? w.items[i].customW : w.itemW;
                float currentItemH = (!isDummy && w.items[i].customH > 0.0f) ? w.items[i].customH : w.itemH;
                float itemX = 0.0f, itemY = 0.0f;   // تهيئة صريحة: بعض فروع الاتجاه كانت تتركهما بلا قيمة
                if (!isDummy && w.items[i].customX >= 0.0f && w.items[i].customY >= 0.0f) {
                    itemX = w.x + elemOffsetX + w.items[i].customX;
                    itemY = w.y + elemOffsetY + w.items[i].customY - autoScrollY;
                } else {
                    if (w.orientId == OrientId::Vertical) {
                        itemX = w.x + elemOffsetX + (w.w - currentItemW) / 2.0f + w.itemOffsetX; 
                        //itemY = w.y + elemOffsetY + w.itemOffsetY + (i * (w.itemH + gap)) - w.currentScrollOffset;
                        // 🚀 إزالة floor للسماح بنعومة الأنيميشن وحركة السكرول
                        float step = w.itemH + gap; 
                        itemY = w.y + elemOffsetY + w.itemOffsetY + (i * step) - w.currentScrollOffset - autoScrollY;
                    } else if (w.orientId == OrientId::Horizontal) {
                        itemX = w.x + elemOffsetX + w.itemOffsetX + (i * (w.itemW + gap)) - w.currentScrollOffset; 
                        itemX += (w.itemW - currentItemW) / 2.0f; 
                        //itemY = w.y + elemOffsetY + (w.h - w.itemH) / 2.0f + w.itemOffsetY - autoScrollY;
                        itemY = w.y + elemOffsetY + floor((w.h - w.itemH) / 2.0f) + w.itemOffsetY - autoScrollY; 
                    } else if (w.orientId == OrientId::Grid) { // ✅ تمت إضافة حسابات الشبكة هنا
                        int col = i % w.gridColumns;
                        int row = i / w.gridColumns;
                        itemX = w.x + elemOffsetX + w.itemOffsetX + (col * (w.itemW + gap));
                        itemY = w.y + elemOffsetY + w.itemOffsetY + (row * (w.itemH + gap)) - w.currentScrollOffset - autoScrollY;
                    }
                }

                float scale = 1.0f;
                if (w.zoomEnabled && !isDummy) { // لا يوجد تكبير للعناصر الوهمية
                    if (primeCarouselMode) {
                        float focusIndex = w.isAnimating ? w.zoomVisualIndex : (float)w.selectedIndex;
                        float dist = fabs((float)i - focusIndex);
                        float focus = 1.0f - std::min(1.0f, dist);
                        scale = 1.0f + (effZoomFactor - 1.0f) * focus;
                    } else {
                        if ((int)i == w.selectedIndex) {
                            if (w.isAnimating) scale = 1.0f + (effZoomFactor - 1.0f) * tZoom;
                            else scale = effZoomFactor;
                        } else if ((int)i == w.prevIndex && w.isAnimating) {
                            scale = effZoomFactor - (effZoomFactor - 1.0f) * tZoom;
                        }
                    }
                }

                float drawW = currentItemW * scale;
                float drawH = currentItemH * scale;

                float drawX = itemX - (drawW - currentItemW) / 2.0f;
                float drawY = itemY - (drawH - currentItemH) / 2.0f;
                
                // // 🚀 تحرير البوسترات: إيقاف التقطيع (Sub-pixel rendering) أثناء الانزلاق
                // if (!w.isAnimating && !currentScreen.slideActive) {
                //     drawX = floor(drawX - elemOffsetX + 0.5f) + elemOffsetX;
                //     drawY = floor(drawY - elemOffsetY + 0.5f) + elemOffsetY;
                //     drawW = floor(drawW + 0.5f);
                //     drawH = floor(drawH + 0.5f);
                // }

                // ✅ توسيع منطقة الـ Off-screen لضمان رسم البوستر في الذاكرة قبل أن ينزلق للشاشة
                // 🚀 هامش القص: كان 1000px فيُرسم ما يقارب خمسة صفوف خارج الشاشة
                //    فوق وتحت (٧٠ عنصراً بدل ١٥) وهو أثقل شيء في الشبكات النصية.
                if (drawX + drawW < w.x + elemOffsetX - 20.0f || drawX > w.x + elemOffsetX + w.w + 20.0f ||
                    drawY + drawH < w.y + elemOffsetY - 20.0f || drawY > w.y + elemOffsetY + w.h + 20.0f) {
                    continue; 
                }
                // 👆 نهاية الإخفاء الذكي 👆
                
                // ✅ حساب الانحناء (دائرة كاملة إذا كان النوع circle)
                float currentItemRadius = w.itemCornerRadius;
                if (w.itemShape == ItemShapeId::Circle) {
                    currentItemRadius = std::min(drawW, drawH) / 2.0f;
                }

                
                // 🚀 حماية إضافية: رسم الصور والنصوص للعناصر الحقيقية فقط
                if (!isDummy) {
                    // ✅ 1. الفحص الحقيقي والمباشر: هل الصورة مشحونة وجاهزة تماماً في كرت الشاشة؟
                    bool isTextureReady = (w.items[i].loaded && w.items[i].textureId != 0);

                    // ✅ 2. إذا كانت الصورة جاهزة، ارسم البوستر فوراً واقفل الباب (مستحيل يظهر السپينر هنا)
                    if (isTextureReady) {
                        glUniform1f(useTexLoc, 1.0f); 
                        glBindTexture(GL_TEXTURE_2D, w.items[i].textureId);
                        // 🚀 التعديل الجديد: تأثير النبض المستمر (Breathing Skeleton)
                        float finalAlpha = 1.0f * elemAlpha;
                        bool isSkeletonPulse = (w.items[i].imagePath.find("skeleton_pulse") != std::string::npos);
                        if (isSkeletonPulse) {
                            finalAlpha *= (0.6f + 0.4f * sin(currentTime * 5.0f));
                            force_render = true; // إجبار المحرك على الاستمرار لتحديث النبض
                        }
                        bool posterHasAlpha = true;
                        int posterPIdx = w.items[i].poolIndex;
                        if (posterPIdx >= 0 && posterPIdx < MAX_TEXTURE_POOL) {
                            posterHasAlpha = texturePool[posterPIdx].hasAlpha;
                            if (texturePool[posterPIdx].preRounded) {
                                // الزوايا أصبحت موجودة داخل الـ Texture نفسها، فلا نحتاج shader corner الثقيل.
                                // الشكل يبقى rounded أثناء الحركة وبعدها، لكن الرسم يصبح Texture عادية.
                                currentItemRadius = 0.0f;
                            }
                        }
                        if (posterHasAlpha || finalAlpha < 0.999f || isSkeletonPulse) gl_set_blend(true);
                        else gl_set_blend(false);
                        glUniform4f(colorLoc, 1.0f, 1.0f, 1.0f, finalAlpha);
                        glUniform1f(roundLoc, currentItemRadius);
                        glUniform1f(borderWidthLoc, 0.0f);

                        float finalDrawX = drawX;
                        float finalDrawY = drawY;
                        float finalDrawW = drawW;
                        float finalDrawH = drawH;

                        // 🚀 زيادة مساحة الرسم لتتطابق تماماً مع النسب الجديدة لتفادي القطع
                        bool useMotionReflectionCrop = false;
                        float motionTexCoords[] = { 0,0, 1,0, 0,1, 1,1 };
                        if (w.hasReflection) {
                            if (posterMotionLiteMode) {
                                // أثناء حركة الكاروسال/الـ Grid لا نرسم الانعكاس لأنه Alpha Fill-rate ثقيل جداً.
                                // نقرأ فقط الجزء الأصلي من Texture المركبة حتى لا تنضغط الصورة.
                                float vMax = 1.0f / (1.0f + 0.02f + 0.15f);
                                motionTexCoords[4] = 0.0f; motionTexCoords[5] = vMax;
                                motionTexCoords[6] = 1.0f; motionTexCoords[7] = vMax;
                                useMotionReflectionCrop = true;
                            } else {
                                // ✅ الحل: توحيد النسب هنا لتكون مطابقة تماماً (0.02 و 0.15)
                                finalDrawH = drawH + (drawH * 0.02f) + (drawH * 0.15f);
                            }
                        }

                        // الحفاظ على أبعاد الشعارات (Aspect Fit)
                        if (w.name == "vod_title") {
                            int pIdx = w.items[i].poolIndex;
                            if (pIdx != -1 && texturePool[pIdx].width > 0 && texturePool[pIdx].height > 0) {
                                float imgW = texturePool[pIdx].width;
                                float imgH = texturePool[pIdx].height;
                                float targetRatio = drawW / drawH;
                                float imgRatio = imgW / imgH;

                                if (imgRatio > targetRatio) {
                                    finalDrawH = drawW / imgRatio;
                                    finalDrawY = drawY + (drawH - finalDrawH) / 2.0f;
                                } else {
                                    finalDrawW = drawH * imgRatio;
                                    finalDrawX = drawX;
                                }
                            }
                        }

                        float fit_iv[] = { 
                            finalDrawX, finalDrawY, finalDrawX + finalDrawW, finalDrawY, 
                            finalDrawX, finalDrawY + finalDrawH, finalDrawX + finalDrawW, finalDrawY + finalDrawH 
                        };

                        bool useFastPosterPath = (currentItemRadius <= 0.0f) &&
                                                     (posterMotionLiteMode || (posterPIdx >= 0 && posterPIdx < MAX_TEXTURE_POOL && texturePool[posterPIdx].preRounded));
                        if (useFastPosterPath) {
                            // 🚀 Fast path: إما أثناء الحركة، أو لأن الزوايا مخبوزة مسبقاً داخل الـ Texture.
                            gl_use_program(fastProgram);
                            glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(activeProj));
                            glUniform1f(fastUseTexLoc, 1.0f);
                            glUniform4f(fastColorLoc, 1.0f, 1.0f, 1.0f, finalAlpha);
                            glBindTexture(GL_TEXTURE_2D, w.items[i].textureId);
                            glEnableVertexAttribArray(fastPosLoc); glEnableVertexAttribArray(fastTexCoordLoc);
                            glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, fit_iv);
                            glVertexAttribPointer(fastTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, useMotionReflectionCrop ? motionTexCoords : texCoords);
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                            // الرجوع للبرنامج الأساسي لرسم النصوص/البادج/الأيقونات التالية.
                            gl_use_program(program);
                            glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(activeProj));
                            glEnableVertexAttribArray(posLoc); glEnableVertexAttribArray(texCoordLoc);
                            glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
                        } else {
                            glUniform2f(rectPosLoc, finalDrawX, finalDrawY);
                            glUniform2f(boxSizeLoc, finalDrawW, finalDrawH);
                            glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, fit_iv);
                            if (useMotionReflectionCrop) {
                                glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, motionTexCoords);
                            }
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                            if (useMotionReflectionCrop) {
                                glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
                            }
                        }

                        // 🚀 رسم مدة الفيديو (Duration Badge) أسفل يمين البوستر.
                        // أصبح Texture واحدة جاهزة، لذلك لا نخفيه أثناء حركة horizontal/grid حتى لا يظهر/يختفي.
                        if (w.items[i].badgeTexId != 0 && w.items[i].imagePath.find("skeleton_pulse") == std::string::npos) {
                            gl_set_blend(true);
                            glUniform1f(useTexLoc, 1.0f);
                            glBindTexture(GL_TEXTURE_2D, w.items[i].badgeTexId);
                            glUniform4f(colorLoc, 1.0f, 1.0f, 1.0f, 1.0f * elemAlpha);
                            glUniform1f(roundLoc, 0.0f);
                            glUniform1f(borderWidthLoc, 0.0f);
                            float padX = 6.0f * scale; float padY = 6.0f * scale;
                            float badgeW = w.items[i].badgeW * scale;
                            float badgeH = w.items[i].badgeH * scale;
                            float badgeX = finalDrawX + finalDrawW - badgeW - padX;
                            float actualDrawH = w.hasReflection ? drawH : finalDrawH;
                            float badgeY = finalDrawY + actualDrawH - badgeH - padY;
                            float bv[] = { badgeX, badgeY, badgeX+badgeW, badgeY, badgeX, badgeY+badgeH, badgeX+badgeW, badgeY+badgeH };
                            glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, bv);
                            glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        }
                    } 
                    // ✅ 3. إذا لم تكن الصورة جاهزة (قيد التحميل)، ارسم السپينر "فقط وفقط" للعنصر المحدد حالياً
                    // else if (!w.items[i].loaded && (int)i == w.selectedIndex && w.items[i].imagePath != "none" && !w.items[i].imagePath.empty()) {
                    else if (false) {    
                        // إخفاء السپينر أثناء حركة الشاشة لتجنب وميضه (Blink) عندما يكون التحميل من الكاش سريعاً
                        if (!currentScreen.slideActive && !w.isAnimating) {
                            if (texLoadingBack != 0 && texLoadingSpinner != 0) {
                                gl_set_blend(true); 
                                glUniform1f(useTexLoc, 1.0f); 
                                glUniform4f(colorLoc, 1.0f, 1.0f, 1.0f, 1.0f * elemAlpha);
                            
                            // 📐 حساب مقاس السپينر وتمركزه في منتصف الصندوق تماماً
                            float spW = 80.0f * scale; 
                            float spH = 80.0f * scale;
                            float spX = drawX + (drawW - spW) / 2.0f;
                            float spY = drawY + (drawH - spH) / 2.0f;
                            
                            if (!w.items[i].text.empty()) {
                                spY -= (15.0f * scale);
                            }
                            
                            // أ) رسم الخلفية الثابتة للسبينر
                            glBindTexture(GL_TEXTURE_2D, texLoadingBack);
                            float sv[] = { spX, spY, spX + spW, spY, spX, spY + spH, spX + spW, spY + spH };
                            glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, sv);
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                            // ب) رسم الجزء المتحرك (الدوران)
                            glBindTexture(GL_TEXTURE_2D, texLoadingSpinner);
                            float cx = spX + spW / 2.0f; 
                            float cy = spY + spH / 2.0f;
                            float angle = currentTime * 6.0f; 
                            float s = sin(angle); 
                            float c = cos(angle);
                            
                            auto rx = [&](float x, float y) { return cx + (x - cx) * c - (y - cy) * s; };
                            auto ry = [&](float x, float y) { return cy + (x - cx) * s + (y - cy) * c; };
                            
                            float rv[] = { 
                                rx(spX, spY),       ry(spX, spY), 
                                rx(spX+spW, spY),   ry(spX+spW, spY), 
                                rx(spX, spY+spH),   ry(spX, spY+spH), 
                                rx(spX+spW, spY+spH), ry(spX+spW, spY+spH) 
                            };
                            glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, rv);
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        }
                        }
                    }

                // ==========================================================================
                // 📊 3.5 شريط التقدّم المدمج (Native Progress Bar)
                //    يُرسم فوق البوستر/الخلفية وتحت النصوص والأيقونات.
                //    القيمة تأتي من الحقل رقم 10 في سطر العنصر أو من set_widget_item_progress.
                // ==========================================================================
                if (w.progressEnabled && w.progW > 0.0f && w.progH > 0.0f && w.items[i].progress >= 0.0f) {
                    float pct = w.items[i].progress;
                    if (pct > 100.0f) pct = 100.0f;

                    float barX = drawX + (w.progX * scale);
                    float barY = drawY + (w.progY * scale);
                    float barW = w.progW * scale;
                    float barH = w.progH * scale;

                    float barR = w.progCornerRadius * scale;
                    if (barR > barH * 0.5f) barR = barH * 0.5f;   // لا يتجاوز نصف السماكة
                    if (barR < 0.0f) barR = 0.0f;

                    bool  progSelected = (w.showSelection && (int)i == w.selectedIndex);

                    float ptR = w.progTrackR, ptG = w.progTrackG, ptB = w.progTrackB, ptA = w.progTrackA;
                    if (progSelected && w.progTrackSelR >= 0.0f) {
                        ptR = w.progTrackSelR; ptG = w.progTrackSelG; ptB = w.progTrackSelB; ptA = w.progTrackSelA;
                    }
                    float pfR = w.progFillR, pfG = w.progFillG, pfB = w.progFillB, pfA = w.progFillA;
                    if (progSelected && w.progFillSelR >= 0.0f) {
                        pfR = w.progFillSelR; pfG = w.progFillSelG; pfB = w.progFillSelB; pfA = w.progFillSelA;
                    }

                    gl_set_blend(true);
                    glUniform1f(useTexLoc, 0.0f);
                    glUniform1f(gradientModeLoc, 0.0f);
                    glUniform1f(borderWidthLoc, 0.0f);
                    glUniform1f(roundLoc, barR);

                    // أ) المسار (الخلفية الرمادية)
                    if (ptA > 0.01f) {
                        glUniform4f(colorLoc, ptR, ptG, ptB, ptA * elemAlpha);
                        glUniform2f(rectPosLoc, barX, barY);
                        glUniform2f(boxSizeLoc, barW, barH);
                        float ptv[] = { barX, barY, barX + barW, barY, barX, barY + barH, barX + barW, barY + barH };
                        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, ptv);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    }

                    // ب) الجزء الممتلئ
                    float fillW = barW * (pct / 100.0f);
                    if (fillW > 0.5f && pfA > 0.01f) {
                        glUniform4f(colorLoc, pfR, pfG, pfB, pfA * elemAlpha);
                        glUniform2f(rectPosLoc, barX, barY);
                        glUniform2f(boxSizeLoc, fillW, barH);
                        float pfv[] = { barX, barY, barX + fillW, barY, barX, barY + barH, barX + fillW, barY + barH };
                        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, pfv);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    }

                    glUniform1f(roundLoc, 0.0f);   // تنظيف الحالة قبل رسم النصوص
                }

                // ✅ 4. رسم النص فوق المربع (إذا كان موجوداً)
                // 🚀 لا نخفي النصوص/الأيقونات أثناء حركة horizontal carousel.
                // التقطيع السابق كان من التوليد والـ shaders، أما إخفاء النصوص سبب ظهور/اختفاء مزعج.
                bool skipOverlayDuringMotion = false;
                // 🚀 تحديث ذكي: إظهار النص للعنصر الأول فقط أثناء التحميل (بسطر واحد لتفادي أخطاء الأقواس)
                if (!skipOverlayDuringMotion && w.items[i].textTexId != 0 && !(w.items[i].imagePath == "loading_state" && i != 0)) {
                    gl_set_blend(true);
                    glUniform1f(useTexLoc, 1.0f);
                    glBindTexture(GL_TEXTURE_2D, w.items[i].textTexId);
                    
                   // ✅ حساب اللون الديناميكي (عادي ومحدد) مع مزج (Mix) ناعم جداً أثناء الأنيميشن
                    float tR = w.fgR, tG = w.fgG, tB = w.fgB;
                    
                    // 🚀 السحر هنا: لا نطبق ألوان التحديد إلا إذا كان الويدجت في وضع الفوكس
                    if (w.showSelection) {
                        float tColor = w.zoomEnabled ? tZoom : tSelection;
                        if (!w.selectionpixmapAnim) {
                            tColor = 1.0f;
                        }

                        if ((int)i == w.selectedIndex) {
                            if (w.isAnimating) {
                                tR = w.fgR + (w.fgSelR - w.fgR) * tColor;
                                tG = w.fgG + (w.fgSelG - w.fgG) * tColor;
                                tB = w.fgB + (w.fgSelB - w.fgB) * tColor;
                            } else {
                                tR = w.fgSelR; tG = w.fgSelG; tB = w.fgSelB;
                            }
                        } else if ((int)i == w.prevIndex && w.isAnimating) {
                            tR = w.fgSelR - (w.fgSelR - w.fgR) * tColor;
                            tG = w.fgSelG - (w.fgSelG - w.fgG) * tColor;
                            tB = w.fgSelB - (w.fgSelB - w.fgB) * tColor;
                        }
                    }

                    glUniform4f(colorLoc, tR, tG, tB, 1.0f * elemAlpha);
                    glUniform1f(roundLoc, 0.0f); 
                    glUniform1f(borderWidthLoc, 0.0f);
                    glUniform1f(gradientModeLoc, 0.0f); // 🚀 إجبار تصفير التدرج قبل رسم أي نص
                    
                    float txtW = w.items[i].textW * scale; 
                    float txtH = w.items[i].textH * scale;
                    
                    // 🚀 استدعاء أبعاد السطر الأول
                    float firstLineW = w.items[i].textFirstLineW * scale;
                    float firstLineH = w.items[i].textFirstLineH * scale;
                    if (firstLineH <= 0.0f) firstLineH = txtH;
                    
                    // ✅ أقصى عرض مسموح للنص داخل المربع
                    float maxTxtW = drawW - (w.itemTextOffsetX * scale * 2.0f);
                    if (w.itemTextMaxW > 0.0f) {
                        maxTxtW = w.itemTextMaxW * scale;
                    }
                    if (maxTxtW <= 0.0f) maxTxtW = drawW;

                    // 🚀 حساب نقطة بداية "نافذة القص" بناءً على المحاذاة
                    std::string align = (!w.items[i].customAlign.empty()) ? w.items[i].customAlign : w.itemTextAlign;
                    float windowX = drawX + (w.itemTextOffsetX * scale); // افتراضي لليسار
                    if (align == "center") {
                        windowX = drawX + (drawW - maxTxtW) / 2.0f;
                    } else if (align == "right") {
                        windowX = drawX + drawW - maxTxtW - (w.itemTextOffsetX * scale);
                    }

                    // ✅ حساب الإحداثي X للنص
                    float txtX = drawX;
                    if (align == "left" || align == "block") {
                        txtX = windowX;
                    } else if (align == "right") {
                        txtX = drawX + drawW - txtW - (w.itemTextOffsetX * scale);
                        if (txtW > maxTxtW) txtX = windowX; // يبدأ من النافذة المحددة عند السكرول
                    } else { // center
                        txtX = drawX + (drawW - txtW) / 2.0f;
                        if (txtW > maxTxtW) {
                            txtX = windowX; // 🚀 السحر هنا: يبدأ من حافة النافذة الوسطية بدلاً من اليسار المطلق
                        } 
                    }
                    
                    float startX = txtX;
                    bool showDots = false;
                    (void)showDots;   // محجوز لميزة "..." عند اقتطاع النص



                    float txtY = drawY + (drawH - txtH) / 2.0f + (w.textOffsetY * scale);
                    
                    // 🚀 السحر هنا: إزاحة النص الثابت للأسفل لكي لا يتداخل مع الدائرة المتحركة
                    if (w.items[i].imagePath == "loading_state") {
                        txtY += (40.0f * scale);
                    }

                    float firstLineX = startX; // 👈 متغير جديد لحركة السطر الأول فقط

                    // 🚀 --- تجهيز ورسم الأفاتار (الصورة الشخصية) --- 🚀
                    float avatarSize = 36.0f * scale; // حجم الصورة
                    float avatarPad = 12.0f * scale;  // الفراغ بينها وبين النص
                    bool hasAvatar = (w.items[i].avatarTexId != 0);
                    float textShift = hasAvatar ? (avatarSize + avatarPad) : 0.0f;
                    float originalStartX = startX;

                    if (hasAvatar) {
                        maxTxtW -= textShift;
                        if (maxTxtW < 0.0f) maxTxtW = 0.0f;
                        txtX += textShift;
                        firstLineX += textShift;
                        startX += textShift;
                        windowX += textShift; // إزاحة نافذة القص أيضاً
                    }

                    if (hasAvatar) {
                        glUniform1f(useTexLoc, 1.0f);
                        glBindTexture(GL_TEXTURE_2D, w.items[i].avatarTexId);
                        glUniform4f(colorLoc, 1.0f, 1.0f, 1.0f, 1.0f * elemAlpha);
                        glUniform1f(roundLoc, avatarSize / 2.0f); // 👈 السحر هنا: قص الصورة لتصبح دائرية تماماً!
                        glUniform1f(borderWidthLoc, 0.0f);
                        glUniform1f(gradientModeLoc, 0.0f);
                        
                        float firstLineH_av = w.items[i].textFirstLineH * scale;
                        if (firstLineH_av <= 0.0f) firstLineH_av = txtH;
                        
                        // 🚀 إصلاح اهتزاز الأفاتار: تقريب الإحداثيات على المحورين لضمان استقرار البكسل
                        float avX = floor(originalStartX - elemOffsetX + 0.5f) + elemOffsetX; 
                        float avY = floor(txtY - elemOffsetY + 0.5f) + elemOffsetY + floor((firstLineH_av - avatarSize) / 2.0f + 0.5f); 
                        
                        float avV[] = { avX, avY, avX + avatarSize, avY, avX, avY + avatarSize, avX + avatarSize, avY + avatarSize };
                        glUniform2f(rectPosLoc, avX, avY);
                        glUniform2f(boxSizeLoc, avatarSize, avatarSize);
                        
                        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, avV);
                        glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords); 
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        
                        glUniform1f(roundLoc, 0.0f); // إعادة ضبط الانحناء لكي لا تتأثر النصوص
                        
                        // 🚀 استرجاع إعدادات لون النص والـ Texture لكي يكمل رسم النص بشكل صحيح
                        glBindTexture(GL_TEXTURE_2D, w.items[i].textTexId);
                        glUniform4f(colorLoc, tR, tG, tB, 1.0f * elemAlpha);
                    }

                    // ✅ نظام تحريك النصوص (Marquee)
                    if (w.scrollTextMode == 1 && firstLineW > maxTxtW) {
                        if (w.showSelection && (int)i == w.selectedIndex) {
                            if (w.textScrollActive) {
                                if (w.textScrollStartTime < 0.0f) w.textScrollStartTime = currentTime;
                                float elapsed = currentTime - w.textScrollStartTime;
                                float delay = w.scrollDelay >= 0.0f ? w.scrollDelay : 0.5f; 
                                if (elapsed > delay) {
                                    float activeTime = elapsed - delay;
                                    float speed = 120.0f; 
                                    float max_scroll_dist = firstLineW - maxTxtW;

                                    if (w.scrollTextStyle == 2) { // 🚀 نمط الارتداد ذهاب وعودة للـ Widget
                                        float T_scroll = max_scroll_dist / speed;
                                        float T_pause = 1.0f;
                                        float T_cycle = T_scroll + T_pause + T_scroll + T_pause;
                                        int current_loop = (int)(activeTime / T_cycle);

                                        if (current_loop >= w.scrollTextShot) {
                                            w.textScrollActive = false;
                                            firstLineX = startX;
                                            showDots = true;
                                        } else {
                                            float time_in_cycle = fmod(activeTime, T_cycle);
                                            if (time_in_cycle < T_scroll) {
                                                firstLineX = startX - (time_in_cycle * speed); // حركة لليسار
                                            } else if (time_in_cycle < T_scroll + T_pause) {
                                                firstLineX = startX - max_scroll_dist; // ثبات عند النهاية
                                            } else if (time_in_cycle < T_scroll + T_pause + T_scroll) {
                                                float t_rev = time_in_cycle - (T_scroll + T_pause);
                                                firstLineX = (startX - max_scroll_dist) + (t_rev * speed); // عودة لليمين
                                            } else {
                                                firstLineX = startX; // ثبات عند البداية
                                            }
                                        }
                                    } else { // 🔄 النمط 1 الافتراضي للـ Widget
                                        float d1 = startX - drawX + firstLineW; 
                                        float L = firstLineW + drawW;          
                                        float D = fmod(activeTime * speed, L);
                                        int loops = (int)((activeTime * speed) / L);
                                        
                                        if (loops >= w.scrollTextShot) { 
                                            w.textScrollActive = false;
                                            firstLineX = startX;
                                            showDots = true;
                                        } else {
                                            if (D < d1) firstLineX = startX - D;
                                            else firstLineX = (drawX + drawW) - (D - d1);
                                        }
                                    }
                                } else { showDots = true; }
                            } else { showDots = true; }
                        } else { showDots = true; }
                    } 
                    else if (firstLineW > maxTxtW) {
                        showDots = true;
                    }
                    // ✅ حساب منطقة القص (Scissor)
                    int wx = (int)(w.x + elemOffsetX - clipPadX);
                    int wy = h - (int)(w.y + elemOffsetY + w.h + clipPadBottom + extraSpaceForText);
                    int ww = (int)(w.w + clipPadX * 2.0f);
                    int wh = (int)(w.h + clipPadY + clipPadBottom + extraSpaceForText);

                    // ✅ حساب منطقة القص للنص بشكل دقيق
                    int ix = (int)windowX; // ✂️ استخدام نقطة النافذة الصحيحة (سواء كانت يسار أو وسط أو يمين)
                    int iw = (int)maxTxtW; // ✂️ استخدام العرض الأقصى للنص بدقة

                    int scX = std::max(wx, ix);
                    int scY = wy;
                    
                    // ✅ تقليل مساحة النص فقط في حالة وجود النقاط
                    int scR_text = std::min(wx + ww, ix + iw);
                    // if (showDots) {
                    //     scR_text -= (int)(18.0f * scale); 
                    // }
                    int scW_text = scR_text - scX;
                    if (scW_text < 0) scW_text = 0;
                    int scT = wy + wh;
                    int scH = scT - scY;
                    if (scH < 0) scH = 0;

                    glEnable(GL_SCISSOR_TEST);
                    glScissor(scX, scY, scW_text, scH);

                    // 🚀 هبوط رياضي مثالي للنصوص لمنع الرمشة عند توقف الأنيميشن تماماً
                    float f_txtY = floor(txtY - elemOffsetY + 0.5f) + elemOffsetY;
                    
                    // 🚀 1. رسم السطر الأول المتحرك (Title)
                    float f_firstX = floor(firstLineX - elemOffsetX + 0.5f) + elemOffsetX;
                    float f_restX  = floor(startX     - elemOffsetX + 0.5f) + elemOffsetX;
                    float vRatio = firstLineH / txtH; // 👈 نسبة ارتفاع السطر الأول للقص

                    // ✅ رسم النص (السطر الأول + باقي الأسطر) بإزاحة اختيارية،
                    //    نستعملها لرسم الظل/الحدّ أولاً ثم النص فوقه.
                    auto emitTextQuads = [&](float dx, float dy) {
                        float tv1[] = { f_firstX+dx, f_txtY+dy, f_firstX+txtW+dx, f_txtY+dy,
                                        f_firstX+dx, f_txtY+firstLineH+dy, f_firstX+txtW+dx, f_txtY+firstLineH+dy };
                        float tc1[] = { 0,0, 1,0, 0,vRatio, 1,vRatio };
                        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, tv1);
                        glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, tc1);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                        // 🚀 2. رسم باقي الأسطر الثابتة (Channel & Views)
                        if (firstLineH < txtH) {
                            float tv2[] = { f_restX+dx, f_txtY+firstLineH+dy, f_restX+txtW+dx, f_txtY+firstLineH+dy,
                                            f_restX+dx, f_txtY+txtH+dy,       f_restX+txtW+dx, f_txtY+txtH+dy };
                            float tc2[] = { 0,vRatio, 1,vRatio, 0,1, 1,1 };
                            glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, tv2);
                            glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, tc2);
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        }
                    };

                    // ✅ الظل/الحدّ: يفصل النص عن أي خلفية (فوكس ملوّن، بوستر، تدرّج)
                    if (w.textShadowMode > 0 && elemAlpha > 0.01f) {
                        glUniform4f(colorLoc, w.textShadowR, w.textShadowG, w.textShadowB, w.textShadowA * elemAlpha);
                        if (w.textShadowMode >= 2) {
                            const float ox[8] = { -1.0f, 1.0f, 0.0f, 0.0f, -1.0f, 1.0f, -1.0f, 1.0f };
                            const float oy[8] = { 0.0f, 0.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f };
                            for (int k = 0; k < 8; ++k)
                                emitTextQuads(ox[k] * w.textShadowOffX * scale, oy[k] * w.textShadowOffY * scale);
                        } else {
                            emitTextQuads(w.textShadowOffX * scale, w.textShadowOffY * scale);
                        }
                        glUniform4f(colorLoc, tR, tG, tB, 1.0f * elemAlpha); // إعادة لون النص الحقيقي
                    }

                    emitTextQuads(0.0f, 0.0f);

                    // ✅ إعادة الكاميرا الافتراضية للـ TexCoords بعد الرسم
                    glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);

                    // ✅ إعادة Scissor الخاص بالـ Widget الأصلي ليكمل رسم باقي العناصر!
                    glScissor(wx, wy, ww, wh);
                }
                // ✅ 5. رسم أيقونة المربع (إذا كانت موجودة) مع الأنيميشن وتغيير اللون
                if (!skipOverlayDuringMotion && w.items[i].iconTexId != 0) {
                    gl_set_blend(true);
                    glUniform1f(useTexLoc, 1.0f);
                    glBindTexture(GL_TEXTURE_2D, w.items[i].iconTexId);
                    
                    // حساب مزج الألوان للأيقونة (عادي / محدد)
                    float iR = w.iconR, iG = w.iconG, iB = w.iconB;
                    
                    // 🚀 حماية الأيقونات أيضاً عند فقدان الفوكس
                    if (w.showSelection) {
                        float tColorIcon = w.zoomEnabled ? tZoom : tSelection;
                        if (!w.selectionpixmapAnim) {
                            tColorIcon = 1.0f;
                        }

                        if ((int)i == w.selectedIndex) {
                            if (w.isAnimating) {
                                iR = w.iconR + (w.iconSelR - w.iconR) * tColorIcon;
                                iG = w.iconG + (w.iconSelG - w.iconG) * tColorIcon;
                                iB = w.iconB + (w.iconSelB - w.iconB) * tColorIcon;
                            } else { iR = w.iconSelR; iG = w.iconSelG; iB = w.iconSelB; }
                        } else if ((int)i == w.prevIndex && w.isAnimating) {
                            iR = w.iconSelR - (w.iconSelR - w.iconR) * tColorIcon;
                            iG = w.iconSelG - (w.iconSelG - w.iconG) * tColorIcon;
                            iB = w.iconSelB - (w.iconSelB - w.iconB) * tColorIcon;
                        }
                    }

                    glUniform4f(colorLoc, iR, iG, iB, 1.0f * elemAlpha);
                    glUniform1f(roundLoc, 0.0f); 
                    glUniform1f(borderWidthLoc, 0.0f);
                    
                    float drawIconW = w.items[i].iconW * scale;
                    float drawIconH = w.items[i].iconH * scale;

                    float iconX = drawX + (drawW - drawIconW) / 2.0f + (w.iconOffsetX * scale);
                    float iconY = drawY + (drawH - drawIconH) / 2.0f;

                    // ✅ السحر هنا: تطبيق الإزاحة للرفع للأعلى فقط إذا كان البوكس يحتوي على نص!
                    // أما البوكسات التي بدون نص (مثل الإعدادات) فستبقى في المنتصف تماماً
                    if (!w.items[i].text.empty()) {
                        iconY += (w.iconOffsetY * scale);
                    }
                    
                    // 🚀 هبوط رياضي مثالي للأيقونات لمنع الرمشة
                    float f_iconX = floor(iconX - elemOffsetX + 0.5f) + elemOffsetX;
                    float f_iconY = floor(iconY - elemOffsetY + 0.5f) + elemOffsetY;
                    
                    float icv[] = { f_iconX, f_iconY, f_iconX + drawIconW, f_iconY, f_iconX, f_iconY + drawIconH, f_iconX + drawIconW, f_iconY + drawIconH };
                    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, icv);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
            } // 🚀🚀🚀 إغلاق شرط الحماية !isDummy 🚀🚀🚀        
            } // <--- القوس الذي يغلق حلقة for للصور
            
            // =========================================================
            // 🚀 رسم إطار التحديد أو الصورة فوق العناصر (Foreground)
            // =========================================================
            if (w.showSelection && w.itemW > 0 && w.itemH > 0 && w.items.size() > 0) {
                if (w.selectionIsBorder) {
                    gl_set_blend(true);
                    glUniform1f(useTexLoc, 0.0f);
                    glUniform4f(colorLoc, w.selBorderR, w.selBorderG, w.selBorderB, w.selBorderA * elemAlpha);
                    glUniform1f(gradientModeLoc, (float)w.selBorderGradientMode);
                    if (w.selBorderGradientMode > 0) {
                        glUniform4f(colorEndLoc, w.selBorderR2, w.selBorderG2, w.selBorderB2, w.selBorderA2 * elemAlpha);
                    }
                    glUniform1f(roundLoc, currentSelRadius);
                    glUniform2f(rectPosLoc, selX - pad, selY - pad);
                    glUniform2f(boxSizeLoc, selW + (pad * 2.0f), selH + (pad * 2.0f));
                    glUniform1f(borderWidthLoc, w.selBorderSize);
                    float iv[] = { selX - pad, selY - pad, selX + selW + pad, selY - pad, selX - pad, selY + selH + pad, selX + selW + pad, selY + selH + pad };
                    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, iv);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glUniform1f(borderWidthLoc, 0.0f);
                    glUniform1f(gradientModeLoc, 0.0f);
                }
                else if (!w.selectionIsColor && w.selectionLoaded && w.selectionTexId != 0) {
                    gl_set_blend(true);
                    glUniform1f(useTexLoc, 1.0f);
                    glBindTexture(GL_TEXTURE_2D, w.selectionTexId);
                    glUniform4f(colorLoc, 1.0f, 1.0f, 1.0f, 1.0f * elemAlpha);
                    glUniform1f(roundLoc, currentSelRadius);
                    glUniform2f(rectPosLoc, selX - pad, selY - pad);
                    glUniform2f(boxSizeLoc, selW + (pad * 2.0f), selH + (pad * 2.0f));
                    float iv[] = { selX - pad, selY - pad, selX + selW + pad, selY - pad, selX - pad, selY + selH + pad, selX + selW + pad, selY + selH + pad };
                    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, iv);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
            }
            
            glDisable(GL_SCISSOR_TEST);

            // ==========================================================================
            // 📏 شريط التمرير (Scrollbar + Slider)
            //    يُرسم بعد إلغاء القص حتى لا تقصّه حدود الويدجت،
            //    ويعتمد على نفس totalSize/viewSize/maxScroll المحسوبة أعلاه.
            // ==========================================================================
            if (w.scrollbarMode != 0 && !w.items.empty()) {
                bool  sbVertical = (w.orientId != OrientId::Horizontal);
                float sbViewLen  = sbVertical ? w.h : w.w;
                bool  sbNeeded   = (totalSize > sbViewLen + 1.0f);

                if (w.scrollbarMode == 2 || sbNeeded) {
                    // ---------- 1) هندسة المسار ----------
                    float trackThick, trackLen;
                    if (sbVertical) {
                        trackThick = (w.scrollbarW > 0.0f) ? w.scrollbarW : 6.0f;
                        trackLen   = (w.scrollbarH > 0.0f) ? w.scrollbarH : w.h;
                    } else {
                        trackLen   = (w.scrollbarW > 0.0f) ? w.scrollbarW : w.w;
                        trackThick = (w.scrollbarH > 0.0f) ? w.scrollbarH : 6.0f;
                    }

                    float trackX, trackY;
                    if (sbVertical) {
                        trackX = w.x + elemOffsetX + w.w - trackThick + w.scrollbarOffX;
                        trackY = w.y + elemOffsetY + ((w.h - trackLen) * 0.5f) + w.scrollbarOffY;
                    } else {
                        trackX = w.x + elemOffsetX + ((w.w - trackLen) * 0.5f) + w.scrollbarOffX;
                        trackY = w.y + elemOffsetY + w.h - trackThick + w.scrollbarOffY;
                    }

                    // ---------- 2) مقاس السلايدر ----------
                    float ratio = (totalSize > 1.0f) ? (sbViewLen / totalSize) : 1.0f;
                    if (ratio > 1.0f) ratio = 1.0f;
                    if (ratio < 0.05f) ratio = 0.05f;

                    float slideThick, slideLen;
                    if (sbVertical) {
                        slideThick = (w.sliderW > 0.0f) ? w.sliderW : trackThick;
                        slideLen   = (w.sliderH > 0.0f) ? w.sliderH : (trackLen * ratio);
                    } else {
                        slideLen   = (w.sliderW > 0.0f) ? w.sliderW : (trackLen * ratio);
                        slideThick = (w.sliderH > 0.0f) ? w.sliderH : trackThick;
                    }
                    if (slideLen > trackLen) slideLen = trackLen;
                    if (slideLen < 8.0f)     slideLen = 8.0f;

                    // ---------- 3) نسبة التقدّم ----------
                    float sbProg = 0.0f;
                    if (maxScroll > 1.0f) sbProg = w.currentScrollOffset / maxScroll;
                    if (sbProg < 0.0f) sbProg = 0.0f;
                    if (sbProg > 1.0f) sbProg = 1.0f;

                    float shownProg = sbProg;
                    if (w.sliderAnim) {
                        if (w.sliderVisualPos < 0.0f) w.sliderVisualPos = sbProg;
                        float sFollow = 1.0f - expf(-16.0f * frameDt);
                        w.sliderVisualPos += (sbProg - w.sliderVisualPos) * sFollow;
                        if (fabs(sbProg - w.sliderVisualPos) < 0.0005f) w.sliderVisualPos = sbProg;
                        else force_render = true;
                        shownProg = w.sliderVisualPos;
                    } else {
                        w.sliderVisualPos = sbProg;
                    }

                    // ---------- 4) التلاشي عند السكون ----------
                    float sbAlpha = 1.0f;
                    if (w.sliderAnim) {
                        if (w.scrollbarLastMoveTime < 0.0f) w.scrollbarLastMoveTime = currentTime;
                        const float SB_HOLD = 1.20f;   // مدة البقاء ظاهراً بعد آخر ضغطة
                        const float SB_FADE = 0.35f;   // مدة التلاشي
                        float idle = currentTime - w.scrollbarLastMoveTime;
                        float target = (idle < SB_HOLD) ? 1.0f : (1.0f - ((idle - SB_HOLD) / SB_FADE));
                        if (target < 0.0f) target = 0.0f;
                        if (target > 1.0f) target = 1.0f;

                        if (w.scrollbarAlpha < target) {
                            // الظهور أسرع من الاختفاء
                            float up = frameDt / 0.15f;
                            w.scrollbarAlpha += up;
                            if (w.scrollbarAlpha > target) w.scrollbarAlpha = target;
                        } else {
                            w.scrollbarAlpha = target;
                        }
                        sbAlpha = w.scrollbarAlpha;
                        if (idle < (SB_HOLD + SB_FADE) || sbAlpha > 0.001f) force_render = true;
                    } else {
                        w.scrollbarAlpha = 1.0f;
                    }

                    // ---------- 5) الرسم ----------
                    if (sbAlpha > 0.01f) {
                        float slideX, slideY;
                        if (sbVertical) {
                            slideX = trackX + ((trackThick - slideThick) * 0.5f);
                            slideY = trackY + ((trackLen - slideLen) * shownProg);
                        } else {
                            slideX = trackX + ((trackLen - slideLen) * shownProg);
                            slideY = trackY + ((trackThick - slideThick) * 0.5f);
                        }

                        gl_set_blend(true);
                        glUniform1f(useTexLoc, 0.0f);
                        glUniform1f(gradientModeLoc, 0.0f);
                        glUniform1f(borderWidthLoc, 0.0f);

                        // أ) المسار
                        if (w.scrollbarA > 0.01f) {
                            float trW = sbVertical ? trackThick : trackLen;
                            float trH = sbVertical ? trackLen   : trackThick;
                            glUniform4f(colorLoc, w.scrollbarR, w.scrollbarG, w.scrollbarB,
                                        w.scrollbarA * elemAlpha * sbAlpha);
                            glUniform1f(roundLoc, w.scrollbarRadius);
                            glUniform2f(rectPosLoc, trackX, trackY);
                            glUniform2f(boxSizeLoc, trW, trH);
                            float tv[] = { trackX, trackY, trackX + trW, trackY,
                                           trackX, trackY + trH, trackX + trW, trackY + trH };
                            glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, tv);
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        }

                        // ب) السلايدر
                        if (w.sliderA > 0.01f) {
                            float slW = sbVertical ? slideThick : slideLen;
                            float slH = sbVertical ? slideLen   : slideThick;
                            glUniform4f(colorLoc, w.sliderR, w.sliderG, w.sliderB,
                                        w.sliderA * elemAlpha * sbAlpha);
                            glUniform1f(roundLoc, w.sliderRadius);
                            glUniform2f(rectPosLoc, slideX, slideY);
                            glUniform2f(boxSizeLoc, slW, slH);
                            float sv2[] = { slideX, slideY, slideX + slW, slideY,
                                            slideX, slideY + slH, slideX + slW, slideY + slH };
                            glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, sv2);
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        }
                    }
                }
            }

            glUniform1f(roundLoc, 0.0f);
            g_perf_text_us.fetch_add((int)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - perfTxtStart).count());
        } // إغلاق for (auto& w : currentScreen.widgets)
        } // إغلاق if (hasWidgets)
        } // إغلاق for (int currentZ : zLayers)

        // ==========================================================
        // ✅ رسم الـ Mask Fade (باستخدام المحرك السريع جداً لتفادي التشنج)
        // ==========================================================
        if (currentScreen.isMaskFading) {
            if (currentScreen.maskFadeStartTime < 0.0f) currentScreen.maskFadeStartTime = currentTime;
            float elapsed = currentTime - currentScreen.maskFadeStartTime;
            float rawT = fmin(1.0f, elapsed / currentScreen.maskFadeDuration);

            float maskAlpha = 1.0f - rawT; // يبدأ أسود 100% ويتلاشى للشفاف
            if (rawT >= 1.0f) currentScreen.isMaskFading = false;

            if (maskAlpha > 0.0f) {
                gl_set_blend(true);
                gl_use_program(fastProgram); // 👈 السر هنا: 0 حسابات معقدة
                glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(baseProjection));
                glUniform1f(fastUseTexLoc, 0.0f);
                glUniform4f(fastColorLoc, 0.0f, 0.0f, 0.0f, maskAlpha);

                float mX = currentScreen.maskX; float mY = currentScreen.maskY;
                float mW = currentScreen.maskW; float mH = currentScreen.maskH;
                float mv[] = { mX, mY, mX + mW, mY, mX, mY + mH, mX + mW, mY + mH };

                glEnableVertexAttribArray(fastPosLoc);
                glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, mv);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
        }

        // ==========================================================
        // 4️⃣ رابعاً: رسم طبقة الفيد (Fade In / Fade Out) الشاملة للشاشة
        // ==========================================================
        if (currentScreen.screenFadeState != 0) {
            if (currentScreen.screenFadeStartTime < 0.0f) currentScreen.screenFadeStartTime = currentTime;
            float elapsed = currentTime - currentScreen.screenFadeStartTime;
            float rawT = fmin(1.0f, elapsed / currentScreen.screenFadeDuration);
            
            float blackAlpha = 0.0f;
            if (currentScreen.screenFadeState == 1) { // Fade In (من الأسود إلى شفاف)
                blackAlpha = 1.0f - rawT;
                if (rawT >= 1.0f) currentScreen.screenFadeState = 0; 
            } else if (currentScreen.screenFadeState == 2) { // Fade Out (من شفاف إلى أسود)
                blackAlpha = rawT;
                if (rawT >= 1.0f) currentScreen.screenFadeState = 2; // إبقاؤه أسود
            }

            if (blackAlpha > 0.0f) {
                gl_set_blend(true);
                gl_use_program(fastProgram);
                glUniformMatrix4fv(fastProjLoc, 1, GL_FALSE, glm::value_ptr(baseProjection));
                glUniform1f(fastUseTexLoc, 0.0f);
                glUniform4f(fastColorLoc, 0.0f, 0.0f, 0.0f, blackAlpha); 
                float fv[] = { 0, 0, (float)w, 0, 0, (float)h, (float)w, (float)h };
                glEnableVertexAttribArray(fastPosLoc);
                glVertexAttribPointer(fastPosLoc, 2, GL_FLOAT, GL_FALSE, 0, fv);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
        }
        if (buildStaticCacheThisFrame && !staticCachePresented) {
            // انتهى رسم الفريم المستقر داخل الـ FBO؛ الآن نعرضه على الشاشة كصورة واحدة.
            glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo); gl_state_cache_reset();
            staticSceneValid = true;
            staticSceneDirty.store(false);
            draw_static_scene_cache_to_screen(w, h);
        } else if (buildLayerCacheThisFrame && !layerCachePresented) {
            // حماية نادرة: لو لم توجد طبقات أعلى بعد splitZ.
            glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo); gl_state_cache_reset();
            layerSceneValid = true;
            layerSceneDirty.store(false);
            draw_layer_scene_cache_to_screen(w, h);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo); gl_state_cache_reset();
        }

        // ==============================================================================
        // 🪟 تلاشي الطبقة الخارجة بعد pop: لقطتها تُرسم فوق المشهد المستعاد وتخفت.
        // ==============================================================================
        if (popFadeTex != 0 && popFadeDuration > 0.0f && !g_capturing_snapshot) {
            if (popFadeStart < 0.0f) popFadeStart = currentTime;
            float pt = (currentTime - popFadeStart) / popFadeDuration;
            if (pt >= 1.0f) {
                glm_destroy_snapshot_target(popFadeTex, popFadeFbo);
                popFadeStart = -1.0f;
                popFadeDuration = 0.0f;
            } else {
                float sm = pt * pt * (3.0f - 2.0f * pt);   // smoothstep
                glm_draw_snapshot_fullscreen(popFadeTex, 0.0f, 1.0f - sm);
                force_render = true;                       // نضمن استمرار الإطارات حتى النهاية
            }
        }

        // 🔊 شريط الصوت: يُرسم دائماً فوق كل شيء وخارج الـ FBO حتى لا يُخزن في الكاش
        // 🪟 ولا يُخبَّأ داخل لقطة الطبقة، وإلا بقي محفوراً في الخلفية بعد اختفائه.
        if (!g_capturing_snapshot) draw_volume_overlay(w, h, currentTime);

        // --- GLOBAL SPINNER LOGIC ---
        if (!g_capturing_snapshot && texLoadingBack != 0 && texLoadingSpinner != 0) {
            bool isLoading = false;
            for (const auto& wdg : currentScreen.widgets) {
                if (wdg.name == "poster_row") {
                    if (wdg.items.empty()) {
                        isLoading = true;
                    }
                    break;
                }
            }
            if (isLoading) {
                gl_use_program(program);
                gl_set_blend(true);
                
                glm::mat4 proj = glm::ortho(0.0f, (float)w, (float)h, 0.0f, -1.0f, 1.0f);
                glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(proj));
                glUniform1f(useTexLoc, 1.0f); 
                glUniform4f(colorLoc, 1.0f, 1.0f, 1.0f, 1.0f);
                glUniform1f(gradientModeLoc, 0.0f);
                glUniform1f(borderWidthLoc, 0.0f);

                glEnableVertexAttribArray(posLoc);
                glEnableVertexAttribArray(texCoordLoc);
                float vTexCoords[] = { 0,0, 1,0, 0,1, 1,1 };
                glVertexAttribPointer(texCoordLoc, 2, GL_FLOAT, GL_FALSE, 0, vTexCoords);
                
                float scale = 2.5f; // INCREASED SPINNER SIZE 
                float spW = 80.0f * scale; 
                float spH = 80.0f * scale;
                float spX = (w - spW) / 2.0f;
                float spY = (h - spH) / 2.0f;
                
                glBindTexture(GL_TEXTURE_2D, texLoadingBack);
                float sv[] = { spX, spY, spX + spW, spY, spX, spY + spH, spX + spW, spY + spH };
                glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, sv);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                glBindTexture(GL_TEXTURE_2D, texLoadingSpinner);
                float cx = spX + spW / 2.0f; 
                float cy = spY + spH / 2.0f;
                float angle = currentTime * 6.0f; 
                float s = sin(angle); 
                float c = cos(angle);
                
                auto rx = [&](float x, float y) { return cx + (x - cx) * c - (y - cy) * s; };
                auto ry = [&](float x, float y) { return cy + (x - cx) * s + (y - cy) * c; };
                
                float rv[] = { 
                    rx(spX, spY),       ry(spX, spY), 
                    rx(spX+spW, spY),   ry(spX+spW, spY), 
                    rx(spX, spY+spH),   ry(spX, spY+spH), 
                    rx(spX+spW, spY+spH), ry(spX+spW, spY+spH) 
                };
                glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, rv);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                
                force_render = true;
            }
        }
        // -----------------------------

        glm_present();
        // ✅ تحديث متغير الأنيميشن الذري (للعمال والباثون)
        {
            bool anyAnimating = currentScreen.slideActive;
            for (const auto& w : currentScreen.widgets) {
                if (w.isAnimating || w.localAnimActive) { anyAnimating = true; break; }
            }
            anyWidgetAnimating.store(anyAnimating);
        }
    }

    // ==================================================================================
    // 🪟 نظام الشاشات المتراكبة — التنفيذ
    // ==================================================================================

    // تحرير كل موارد شاشة من كرت الشاشة (تُستعمل عند إغلاق طبقة نهائياً).
    // نسخة مطابقة لما يفعله internal_load_interface_xml_core عند التحميل العادي.
    void glm_release_screen_gpu(ScreenData& s) {
        for (size_t i = 0; i < s.labels.size(); ++i) {
            if (s.labels[i].iconTextureId != 0) glDeleteTextures(1, &s.labels[i].iconTextureId);
            s.labels[i].iconTextureId = 0;
            s.labels[i].textureId = 0;   // ⚠️ ملكية globalTextCache
        }
        for (size_t i = 0; i < s.images.size(); ++i) {
            if (s.images[i].textureId != 0) glDeleteTextures(1, &s.images[i].textureId);
            s.images[i].textureId = 0;
        }
        for (size_t wi = 0; wi < s.widgets.size(); ++wi) {
            WidgetElement& w = s.widgets[wi];
            for (size_t ii = 0; ii < w.items.size(); ++ii) {
                WidgetItem& item = w.items[ii];
                unload_widget_item_texture(item);   // ✅ يحرر خانات المسبح
                item.textTexId  = 0;                // ⚠️ ملكية globalTextCache
                item.badgeTexId = 0;
                if (item.iconTexId   != 0) { glDeleteTextures(1, &item.iconTexId);   item.iconTexId = 0; }
                if (item.avatarTexId != 0) { glDeleteTextures(1, &item.avatarTexId); item.avatarTexId = 0; }
            }
            if (w.selectionTexId != 0) { glDeleteTextures(1, &w.selectionTexId); w.selectionTexId = 0; }
        }
        if (s.currentBg.textureId != 0) { glDeleteTextures(1, &s.currentBg.textureId); s.currentBg.textureId = 0; }
        if (s.oldBg.textureId     != 0) { glDeleteTextures(1, &s.oldBg.textureId);     s.oldBg.textureId = 0; }
        s.labels.clear();
        s.images.clear();
        s.widgets.clear();
        s.gradients.clear();
    }

    // 🧊 تجميد طبقة: نحرر خانات مسبح النسج فقط (300 خانة مشتركة) لأن الطبقة صارت
    //    مجرد صورة. عند الـ pop يعيد manage_widget_textures تحميل المرئي منها تلقائياً
    //    لأن poolIndex عاد -1 و loaded عادت false.
    void glm_freeze_screen_textures(ScreenData& s) {
        for (size_t wi = 0; wi < s.widgets.size(); ++wi) {
            WidgetElement& w = s.widgets[wi];
            for (size_t ii = 0; ii < w.items.size(); ++ii) {
                unload_widget_item_texture(w.items[ii]);
            }
        }
    }

    // 📸 التقاط المشهد الحالي كاملاً (اللقطة السفلية + الطبقة الحيّة) داخل FBO معطى.
    void glm_capture_scene_into(GLuint fbo, int W, int H) {
        invalidate_static_scene_cache();
        invalidate_layer_scene_cache();

        bool   prevCapturing = g_capturing_snapshot;
        GLuint prevTarget    = g_render_target_fbo;

        g_capturing_snapshot = true;
        g_render_target_fbo  = fbo;

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl_state_cache_reset();
        glViewport(0, 0, W, H);

        internal_render_frame_locked(W, H, engine_last_frame_time.load());

        g_capturing_snapshot = prevCapturing;
        g_render_target_fbo  = prevTarget;

        glBindFramebuffer(GL_FRAMEBUFFER, g_render_target_fbo);
        gl_state_cache_reset();

        invalidate_static_scene_cache();
        invalidate_layer_scene_cache();
    }

    // 🗑️ هدم كل الطبقات نهائياً (عند تحميل شاشة جديدة بالكامل أو عند الإغلاق).
    void glm_destroy_all_layers_nolock() {
        for (size_t i = 0; i < screenStack.size(); ++i) {
            glm_release_screen_gpu(screenStack[i].data);
            glm_destroy_snapshot_target(screenStack[i].belowTex, screenStack[i].belowFbo);
        }
        screenStack.clear();
        glm_destroy_snapshot_target(stackSnapTex, stackSnapFbo);
        glm_destroy_snapshot_target(popFadeTex, popFadeFbo);
        popFadeStart = -1.0f;
        popFadeDuration = 0.0f;
        stackDim = 0.0f;
        g_screen_depth.store(0);
    }

    // 🪟 الغلاف العادي: تحميل شاشة جديدة يمحو كل الطبقات (سلوك المحرك القديم تماماً).
    void internal_load_interface_xml(const char* xmlPath, const char* screenName, const char* pluginPath) {
        std::lock_guard<std::mutex> lock(screenMutex);
        glm_destroy_all_layers_nolock();
        internal_load_interface_xml_core(xmlPath, screenName, pluginPath);
    }

    // 🪟 فتح شاشة **فوق** الشاشة الحالية دون إخفائها.
    void internal_push_interface_xml(const char* xmlPath, const char* screenName, const char* pluginPath,
                                     float dim, const char* animType, float animDuration, int fadeIn) {
        std::lock_guard<std::mutex> lock(screenMutex);

        if (!xmlPath || !screenName) return;

        const int W = engine_screen_w;
        const int H = engine_screen_h;

        // تراجع آمن: بلا shader لقطات أو عند بلوغ السقف نتصرّف كتحميل عادي بدل الانهيار.
        if (snapProgram == 0 || (int)screenStack.size() >= MAX_SCREEN_STACK) {
            GLM_LOG("[GLM-STACK] push refused (snapProgram=%u depth=%d) -> normal load\n",
                    snapProgram, (int)screenStack.size());
            glm_destroy_all_layers_nolock();
            internal_load_interface_xml_core(xmlPath, screenName, pluginPath);
            glm_request_render();
            return;
        }

        GLuint newTex = 0, newFbo = 0;
        if (!glm_create_snapshot_target(W, H, newTex, newFbo)) {
            glm_destroy_all_layers_nolock();
            internal_load_interface_xml_core(xmlPath, screenName, pluginPath);
            glm_request_render();
            return;
        }

        // 1) نجمّد المشهد الحالي (بما فيه الطبقات الأدنى) في اللقطة الجديدة.
        glm_capture_scene_into(newFbo, W, H);

        // 2) ندفع الطبقة الحيّة للأسفل مع لقطتها السفلية القديمة.
        StackedLayer layer;
        layer.belowTex = stackSnapTex;
        layer.belowFbo = stackSnapFbo;
        layer.belowDim = stackDim;

        // 🔑 نلتقط الحالة العامة **قبل** internal_load_interface_xml_core لأنه يصفّرها.
        {
            std::lock_guard<std::mutex> vlock(volumeStyleMutex);
            layer.savedVolumeStyle = volumeStyle;
        }
        layer.savedVolumeFromPython = volume_style_from_python.load();
        layer.savedBackdropHidden   = backdrop_hidden.load();
        layer.savedUiHidden         = ui_hidden.load();

        const bool layer_backdrop_hidden = layer.savedBackdropHidden;
        const bool layer_ui_hidden        = layer.savedUiHidden;

        layer.data = std::move(currentScreen);
        screenStack.push_back(std::move(layer));

        stackSnapTex = newTex;
        stackSnapFbo = newFbo;
        stackDim     = (dim < 0.0f) ? 0.0f : (dim > 1.0f ? 1.0f : dim);

        // 3) الطبقة المجاورة مباشرةً تحتفظ بنسجها (pop فوري بلا رمشة)،
        //    وما هو أعمق منها يُفرِّغ خانات المسبح المشتركة (300 خانة).
        if (screenStack.size() > 1) {
            glm_freeze_screen_textures(screenStack[screenStack.size() - 2].data);
        }

        // 4) نبني الشاشة الجديدة في currentScreen النظيف.
        currentScreen = ScreenData();
        internal_load_interface_xml_core(xmlPath, screenName, pluginPath);

        // 🎬 الطبقة العليا ترث حالة نافذة الفيديو من الشاشة التي تحتها.
        //    core load صفّرهما للتو، ولو تركناهما صفراً لاختفى الفيديو خلف الطبقة
        //    ثم بقي البفر معتماً بعد pop لأن لا أحد يعيدهما.
        backdrop_hidden.store(layer_backdrop_hidden);
        ui_hidden.store(layer_ui_hidden);
        last_backdrop_hidden.store(layer_backdrop_hidden);

        // 5) أنيميشن الدخول (يستعمل نفس منظومة الأنيميشن الموجودة).
        if (animType && *animType && std::string(animType) != "none") {
            currentScreen.animType      = animType;
            currentScreen.slideActive   = true;
            currentScreen.slideStartTime = -1.0f;
            if (animDuration > 0.0f) currentScreen.animDuration = animDuration;
        }
        if (fadeIn) {
            currentScreen.screenFadeState     = 1;   // ظهور من الأسود
            currentScreen.screenFadeStartTime = -1.0f;
            if (animDuration > 0.0f) currentScreen.screenFadeDuration = animDuration;
        }

        // ✅ إن لم تُسلَّح حركة دخول داخل الـ push نفسه، نعلّق العرض حتى يصل
        //    trigger_global_animation (أو تنتهي المهلة) فلا يُرسم إطار وسطي.
        {
            bool armedHere = (animType && *animType && std::string(animType) != "none") || (fadeIn != 0);
            g_push_hold = !armedHere;
            g_push_hold_start = -1.0f;
        }

        g_screen_depth.store((int)screenStack.size());
        invalidate_static_scene_cache();
        invalidate_layer_scene_cache();
        glm_request_render();
        GLM_LOG("[GLM-STACK] pushed '%s' -> depth=%d\n", screenName, (int)screenStack.size());
    }

    // 🪟 إغلاق الطبقة العليا والعودة للتي تحتها بحالتها الكاملة.
    void internal_pop_interface_xml(float fadeDuration) {
        std::lock_guard<std::mutex> lock(screenMutex);
        g_push_hold = false;
        if (screenStack.empty()) return;

        const int W = engine_screen_w;
        const int H = engine_screen_h;

        // تلاشٍ ناعم: نلتقط المشهد الحالي ثم نرسمه فوق المشهد المستعاد وهو يخفت.
        if (fadeDuration > 0.0f && snapProgram != 0) {
            glm_destroy_snapshot_target(popFadeTex, popFadeFbo);
            if (glm_create_snapshot_target(W, H, popFadeTex, popFadeFbo)) {
                glm_capture_scene_into(popFadeFbo, W, H);
                popFadeStart    = -1.0f;
                popFadeDuration = fadeDuration;
            } else {
                popFadeDuration = 0.0f;
            }
        }

        // إغلاق الطبقة العليا نهائياً وتحرير مواردها.
        glm_release_screen_gpu(currentScreen);
        glm_destroy_snapshot_target(stackSnapTex, stackSnapFbo);

        // استعادة الطبقة التي تحتها بحالتها كما كانت.
        StackedLayer& top = screenStack.back();
        stackSnapTex  = top.belowTex;
        stackSnapFbo  = top.belowFbo;
        stackDim      = top.belowDim;

        // 🔑 إعادة شكل شريط الصوت الخاص بهذه الشاشة
        {
            std::lock_guard<std::mutex> vlock(volumeStyleMutex);
            volumeStyle = top.savedVolumeStyle;
        }
        volume_style_from_python.store(top.savedVolumeFromPython);

        // 🎬 إعادة ثقب الفيديو كما كان قبل الدفع
        backdrop_hidden.store(top.savedBackdropHidden);
        ui_hidden.store(top.savedUiHidden);
        last_backdrop_hidden.store(top.savedBackdropHidden);   // بلا fade كاذب

        currentScreen = std::move(top.data);
        screenStack.pop_back();

        // 🎭 إعادة القناع العام ليتطابق مع الشاشة المستعادة، ثم الفحص الحاسم:
        //    إن كان الباكدروب المعروض قد خُبز بقناع لا يخص هذه الشاشة (لأن الطلب
        //    اكتمل بينما كانت الطبقة العليا فعّالة) نعيد خبزه فوراً بقناعها الصحيح.
        //    بدون هذا يبقى الباكدروب بلا mask_normal حتى تغيير القسم بالكامل.
        {
            std::lock_guard<std::mutex> qlock(queueMutex);
            backdropMaskPath = currentScreen.maskPath;

            if (currentScreen.currentBg.loaded &&
                !currentScreen.currentBg.imagePath.empty() &&
                currentScreen.bgBakedMask != currentScreen.maskPath) {
                bgTaskPath     = currentScreen.currentBg.imagePath;
                bgTaskMaskPath = currentScreen.maskPath;
                bgTaskPending  = true;
                loaderCv.notify_one();
                GLM_LOG("[GLM-STACK] rebaking backdrop with owner mask: %s\n",
                        currentScreen.maskPath.c_str());
            }
        }


        // إن كانت هناك طبقة صارت الآن مجاورةً مباشرةً، نعيد لها نسجها عند الرسم تلقائياً.
        g_screen_depth.store((int)screenStack.size());

        // إعادة تسليح حالة الرسم: الشاشة المستعادة لم تُرسم منذ تجميدها.
        currentScreen.slideActive       = false;
        currentScreen.slideStartTime    = -1.0f;
        currentScreen.screenFadeState   = 0;
        currentScreen.screenFadeStartTime = -1.0f;

        invalidate_static_scene_cache();
        invalidate_layer_scene_cache();
        glm_request_render();
        GLM_LOG("[GLM-STACK] popped -> depth=%d\n", (int)screenStack.size());
    }

    // 🪟 العودة إلى الشاشة الجذر دفعة واحدة.
    void internal_pop_all_screens(float fadeDuration) {
        std::lock_guard<std::mutex> lock(screenMutex);
        if (screenStack.empty()) return;

        const int W = engine_screen_w;
        const int H = engine_screen_h;

        if (fadeDuration > 0.0f && snapProgram != 0) {
            glm_destroy_snapshot_target(popFadeTex, popFadeFbo);
            if (glm_create_snapshot_target(W, H, popFadeTex, popFadeFbo)) {
                glm_capture_scene_into(popFadeFbo, W, H);
                popFadeStart    = -1.0f;
                popFadeDuration = fadeDuration;
            } else {
                popFadeDuration = 0.0f;
            }
        }

        glm_release_screen_gpu(currentScreen);
        glm_destroy_snapshot_target(stackSnapTex, stackSnapFbo);

        // نتخلص من كل الطبقات فوق الجذر
        while (screenStack.size() > 1) {
            glm_release_screen_gpu(screenStack.back().data);
            glm_destroy_snapshot_target(screenStack.back().belowTex, screenStack.back().belowFbo);
            screenStack.pop_back();
        }

        StackedLayer& root = screenStack.back();
        stackSnapTex  = root.belowTex;
        stackSnapFbo  = root.belowFbo;
        stackDim      = root.belowDim;

        // 🔑 إعادة شكل شريط الصوت الخاص بالشاشة الجذر
        {
            std::lock_guard<std::mutex> vlock(volumeStyleMutex);
            volumeStyle = root.savedVolumeStyle;
        }
        volume_style_from_python.store(root.savedVolumeFromPython);

        // 🎬 إعادة ثقب الفيديو كما كان في الشاشة الجذر
        backdrop_hidden.store(root.savedBackdropHidden);
        ui_hidden.store(root.savedUiHidden);
        last_backdrop_hidden.store(root.savedBackdropHidden);

        currentScreen = std::move(root.data);
        screenStack.pop_back();

        // 🎭 إعادة القناع العام ليتطابق مع الشاشة المستعادة، ثم الفحص الحاسم:
        //    إن كان الباكدروب المعروض قد خُبز بقناع لا يخص هذه الشاشة (لأن الطلب
        //    اكتمل بينما كانت الطبقة العليا فعّالة) نعيد خبزه فوراً بقناعها الصحيح.
        //    بدون هذا يبقى الباكدروب بلا mask_normal حتى تغيير القسم بالكامل.
        {
            std::lock_guard<std::mutex> qlock(queueMutex);
            backdropMaskPath = currentScreen.maskPath;

            if (currentScreen.currentBg.loaded &&
                !currentScreen.currentBg.imagePath.empty() &&
                currentScreen.bgBakedMask != currentScreen.maskPath) {
                bgTaskPath     = currentScreen.currentBg.imagePath;
                bgTaskMaskPath = currentScreen.maskPath;
                bgTaskPending  = true;
                loaderCv.notify_one();
                GLM_LOG("[GLM-STACK] rebaking backdrop with owner mask: %s\n",
                        currentScreen.maskPath.c_str());
            }
        }


        g_screen_depth.store(0);
        currentScreen.slideActive         = false;
        currentScreen.slideStartTime      = -1.0f;
        currentScreen.screenFadeState     = 0;
        currentScreen.screenFadeStartTime = -1.0f;

        invalidate_static_scene_cache();
        invalidate_layer_scene_cache();
        glm_request_render();
        GLM_LOG("[GLM-STACK] popped all -> depth=0\n");
    }

    void internal_set_layer_dim(float dim) {
        std::lock_guard<std::mutex> lock(screenMutex);
        stackDim = (dim < 0.0f) ? 0.0f : (dim > 1.0f ? 1.0f : dim);
        invalidate_static_scene_cache();
        invalidate_layer_scene_cache();
        glm_request_render();
    }


    void internal_set_widget_selection(const char* widgetName, int newIndex) {
        std::lock_guard<std::mutex> lock(screenMutex);
        if (!widgetName) return;
        // 👇 أضف هذين السطرين هنا
        invalidate_layer_scene_cache();
        invalidate_static_scene_cache();
        for (auto& w : currentScreen.widgets) {
            if (w.name == widgetName) {
                int maxIndex = w.items.size() - 1;
                if (maxIndex < 0) break;
                
                if (newIndex < 0) newIndex = 0;
                if (newIndex > maxIndex) newIndex = maxIndex;

                if (newIndex == w.selectedIndex) {
                    break;
                }

                // 🚀 ملاحظة مهمة:
                // لا نعيد الباكدروب تلقائياً من هنا، لأن بعض البلوجينات ترسل set_widget_selection
                // أثناء بدء التريلر أو تحديث الفوكس، وهذا كان يعيد الباكدروب فوراً ويمنع mask_trailer من الظهور.
                // الرجوع من التريلر أو التنقل لفيلم آخر يجب أن يتم من بايثون باستدعاء set_backdrop_hidden(0).

                // 🚀 اكتشاف إذا كان الانتقال عبارة عن دوران (Wrap Around) من الأول للأخير أو العكس
                //bool isWrapAround = ((w.selectedIndex == 0 && newIndex == maxIndex) || (w.selectedIndex == maxIndex && newIndex == 0));
                bool isWrapAround = (maxIndex > 1) && ((w.selectedIndex == 0 && newIndex == maxIndex) || (w.selectedIndex == maxIndex && newIndex == 0));

                bool primeTargetWidget = false;
                if (w.orientId == OrientId::Horizontal && w.itemW >= 80.0f && w.itemH >= 80.0f) {
                    for (const auto& it : w.items) {
                        if (!it.imagePath.empty() && it.imagePath != "none" && it.imagePath != "loading_state") { primeTargetWidget = true; break; }
                    }
                }
                bool rapidRetarget = (primeTargetWidget && w.isAnimating); // ضغط متكرر قبل انتهاء حركة كاروسال بوسترات فقط
                if (primeTargetWidget && !rapidRetarget) {
                    // بداية حركة جديدة لكاروسال البوسترات: المؤشر البصري يبدأ من العنصر الحالي.
                    w.visualIndex = (float)w.selectedIndex; w.zoomVisualIndex = (float)w.selectedIndex;;
                }
                w.prevIndex = w.selectedIndex;
                w.selectedIndex = newIndex;
                // 🚀 إلغاء انزلاق القائمة بالكامل في حال الدوران لمنع خروج القوائم عن حدود الشاشة
                if (isWrapAround) {
                    w.isAnimating = false;
                    anyWidgetAnimating.store(false); 
                    w.prevIndex = newIndex; // إيقاف الزووم أيضاً لهذه القفزة
                    w.visualIndex = (float)newIndex; w.zoomVisualIndex = (float)newIndex;;
                } else {
                    // 🚀 إصلاح الاهتزاز: إذا كان الويدجت لا يحتاج أنيميشن بصري فعلي
                    // (لا زووم + لا انزلاق تحديد + لا كاروسال بوسترات) نطبقه فورياً بلا أنيميشن
                    // ✅ إصلاح جذري: حركة الكاروسال (انزلاق الكاميرا/المحتوى) مستقلة تماماً
                    //    عن selectionpixmapAnim. هذه الخاصية مسؤولة فقط عن انزلاق إطار
                    //    التحديد نفسه، أما تمرير المحتوى (grid / vertical / horizontal)
                    //    فيجب أن يبقى ناعماً سواء فُعّلت الخاصية أو عُطّلت.
                    //    بدون هذا كانت الشبكة (grid) التي فيها selectionpixmapAnim="0"
                    //    تقفز فجأة بينما الطبقات الأخرى (نصوص/أشرطة) تنزلق بنعومة.
                    bool contentScrolls = false;
                    {
                        int   cnt  = (int)w.items.size();
                        float gapL = w.itemGap;
                        bool  isGridL = (w.orientation == "grid");
                        bool  isVertL = (isGridL || w.orientation == "vertical");
                        float totalSizeL = 0.0f;
                        if (isGridL) {
                            int cols = (w.gridColumns > 0) ? w.gridColumns : 1;
                            int rows = (cnt + cols - 1) / cols;
                            totalSizeL = rows * w.itemH + (rows > 0 ? (rows - 1) * gapL : 0.0f);
                        } else if (isVertL) {
                            totalSizeL = cnt * w.itemH + (cnt > 0 ? (cnt - 1) * gapL : 0.0f);
                        } else {
                            totalSizeL = cnt * w.itemW + (cnt > 0 ? (cnt - 1) * gapL : 0.0f);
                        }
                        float viewSizeL = isVertL ? w.h : w.w;
                        contentScrolls = (totalSizeL > viewSizeL + 1.0f);
                    }

                    bool needsVisualAnim = w.zoomEnabled || w.selectionpixmapAnim || primeTargetWidget || contentScrolls;
                    if (needsVisualAnim) {
                        w.isAnimating = true;
                        anyWidgetAnimating.store(true);
                        // Prime-style: لا نعيد ضبط animStartTime مع كل ضغطة؛ الهدف فقط يتغير.
                        w.animStartTime = -1.0f;
                    } else {
                        w.isAnimating = false;
                        anyWidgetAnimating.store(false);
                        w.prevIndex = newIndex;
                        w.visualIndex = (float)newIndex; w.zoomVisualIndex = (float)newIndex;;
                    }
                }

                // ✅ إعادة ضبط السكرول للنص المحدد الجديد
                w.textScrollStartTime = -1.0f;
                w.textScrollActive = true;

                // 📏 إيقاظ شريط التمرير: -1 تعني (أعِد ختم الوقت في الإطار القادم)
                w.scrollbarLastMoveTime = -1.0f;

                // ✅ حفظ موضع التمرير الحالي لحظة الضغط لتكملة الأنيميشن بسلاسة
                w.startScrollOffset = w.currentScrollOffset;
                
                // ✅ تحديث الذاكرة:
                // أثناء الضغط المتتالي لا ندخل manage_widget_textures لأنه يفحص/يفرغ/يشحن عناصر
                // وقد يسبب micro-stutter. سيتم استدعاؤه تلقائياً عند نهاية الحركة داخل render.
                if (!rapidRetarget) {
                    manage_widget_textures(w);
                }
                break;
            }
        }
    }

    // ✅ دالة الانتقال اللحظي بدون أنيميشن (تم إصلاحها برمجياً)
    void internal_set_widget_selection_instant(const char* name, int index) {
        std::lock_guard<std::mutex> lock(screenMutex);
        if (!name) return;
        // 👇 أضف هذين السطرين هنا
        invalidate_layer_scene_cache();
        invalidate_static_scene_cache();
        std::string nameStr(name);
        for (auto& w : currentScreen.widgets) {
            if (w.name == nameStr) {
                w.selectedIndex = index;
                w.prevIndex = index;       // ✅ السحر هنا: نقطة البداية هي نفسها نقطة النهاية
                w.scrollbarLastMoveTime = -1.0f;   // 📏 إيقاظ شريط التمرير
                w.visualIndex = (float)index; w.zoomVisualIndex = (float)index;;
                w.isAnimating = false;     // ✅ إيقاف أي حركة انزلاق أو تكبير فوراً

                // ✅ إعادة ضبط السكرول للنص المحدد الجديد
                w.textScrollStartTime = -1.0f;
                w.textScrollActive = true;
                
                // تحديث الذاكرة فوراً لضمان تحميل الصور إذا قفزت لمكان بعيد
                manage_widget_textures(w);
                break;
            }
        }
    }

    // ✅ دالة جديدة لتغيير مسار وحجم الخط لأي Widget برمجياً
    void set_widget_font(const char* name, const char* fontPath, int fontSize) {
        if (!name || !fontPath) return;
        std::string nameStr(name);
        for (auto& w : currentScreen.widgets) {
            if (w.name == nameStr) {
                w.fontPath = std::string(fontPath);
                if (fontSize > 0) w.fontSize = fontSize;
                break;
            }
        }
    }

    // ✅ دالة لتغيير نوع الأنيميشن لأي عنصر (Widget أو صورة) برمجياً
    void internal_set_element_animation(const char* targetName, const char* animType) {
        if (!targetName || !animType) return;
        std::string t(targetName); std::string a(animType);
        for (auto& w : currentScreen.widgets) { if (w.name == t) w.anim = a; }
        // 🚀 تحديث جذري: دعم البحث بالاسم للصور والنصوص معاً
        for (auto& img : currentScreen.images) { if (img.name == t || img.imagePath.find(t) != std::string::npos) img.anim = a; }
        for (auto& lbl : currentScreen.labels) { if (lbl.name == t) lbl.anim = a; }
    }

    // 🌫️ ضبط تلاشي عنصر بالاسم. mode: 0=بلا، 1=دخول، 2=خروج.
    void internal_set_element_fade(const char* targetName, int mode) {
        if (!targetName) return;
        std::string t(targetName);
        for (auto& w   : currentScreen.widgets)   { if (w.name   == t) w.fadeMode   = mode; }
        for (auto& lbl : currentScreen.labels)    { if (lbl.name == t) lbl.fadeMode = mode; }
        for (auto& img : currentScreen.images) {
            if (img.name == t || (!t.empty() && img.imagePath.find(t) != std::string::npos)) img.fadeMode = mode;
        }
    }

    void internal_set_element_anim_distance(const char* targetName, float dist) {
        if (!targetName) return;
        std::string t(targetName);
        for (auto& w : currentScreen.widgets) { if (w.name == t) w.animDistance = dist; }
        for (auto& img : currentScreen.images) { if (img.name == t || img.imagePath.find(t) != std::string::npos) img.animDistance = dist; }
        for (auto& lbl : currentScreen.labels) { if (lbl.name == t) lbl.animDistance = dist; }
    }

    void internal_set_element_anim_speed(const char* targetName, float speed) {
        if (!targetName || speed <= 0.0f) return;
        std::string t(targetName);
        float duration = 0.8f / speed; // تحويل السرعة إلى وقت زمني
        for (auto& w : currentScreen.widgets) { if (w.name == t) w.animDuration = duration; }
        for (auto& img : currentScreen.images) { if (img.name == t || img.imagePath.find(t) != std::string::npos) img.animDuration = duration; }
        for (auto& lbl : currentScreen.labels) { if (lbl.name == t) lbl.animDuration = duration; }
    }

    // 🚀 دالة جديدة لتشغيل أنيميشن مستقل لويدجت واحد فقط (مثل الأعداد) دون التأثير على الشاشة
    void internal_trigger_widget_custom_anim(const char* widgetName, const char* animType, float duration, float distance) {
        std::lock_guard<std::mutex> lock(screenMutex);
        if (!widgetName || !animType) return;
        std::string wName(widgetName);
        for (auto& w : currentScreen.widgets) {
            if (w.name == wName) {
                w.localAnimActive = true;
                w.localAnimStartTime = -1.0f;
                w.localAnimType = animType;
                w.localAnimDuration = duration > 0.0f ? duration : 0.3f;
                w.localAnimDistance = distance != 0.0f ? distance : 20.0f;
                break;
            }
        }
    }

    // ✅ دالة جديدة للتحكم في إظهار أو إخفاء إطار التحديد (الفوكس)
    void internal_set_widget_focus(const char* name, int isFocused) {
        if (!name) return;
        
        // 🚀 1. إضافة القفل لحماية المسارات ومنع التداخل أثناء تحديث كرت الشاشة
        std::lock_guard<std::mutex> lock(screenMutex); 
        
        std::string n(name);
        for (auto& w : currentScreen.widgets) {
            if (w.name == n) {
                bool wasFocused = w.showSelection;
                w.showSelection = (isFocused != 0);
                
                // إعادة ضبط وقت السكرول ليبدأ من جديد وبشكل نظيف عند استرجاع الفوكس
                if (!wasFocused && w.showSelection) {
                    w.textScrollStartTime = -1.0f;
                    w.textScrollActive = true;
                }
                // تجميد السكرول وإيقافه فوراً عند فقدان الفوكس
                else if (wasFocused && !w.showSelection) {
                    w.textScrollStartTime = -1.0f;
                    w.textScrollActive = false;
                }
                
                // 🚀 2. السحر هنا: إبطال الكاش فوراً لإجبار كرت الشاشة على إعادة بناء المشهد 
                // ورسم الأيقونات والفوكس بالحالة الجديدة لتفادي اختفائها أو تأخر ظهورها
                invalidate_layer_scene_cache();
                invalidate_static_scene_cache();
                
                break;
            }
        }
    }

    // 🚀 دالة ذكية تخبر البايثون إذا كان هناك نص يتحرك حالياً
    bool is_any_text_scrolling() {
        for (const auto& w : currentScreen.widgets) {
            if (w.textScrollActive) {
                return true; // نعم، هناك نص يتحرك!
            }
        }
        return false; // لا يوجد أي نص يتحرك
    }

    // ==================================================================================
    // 🚀 تحديث عنصر واحد داخل ويدجت دون إعادة إرسال القائمة كلها
    // ==================================================================================
    // update_widget_items يعيد تحليل كل السطور ويصفّر التحديد والسكرول، وهذا
    // ثقيل جداً عندما تصل بيانات EPG لعشرة مربعات وسط قائمة فيها آلاف القنوات.
    // هذه الدالة تعدّل نص/محاذاة/مقاس عنصر واحد فقط وتُبقي كل شيء آخر كما هو.
    //   text  : النص الجديد (فارغ = بلا نص)
    //   align : "left" | "center" | "right" | "" (فارغ = لا تغيير)
    //   cw/ch : مقاس مخصص للعنصر (-1 = لا تغيير، يُستعمل لأشرطة التقدّم)
    //   icon  : كود الأيقونة (0 = بلا أيقونة، -1 = لا تغيير)
    // ==================================================================================
    void internal_update_widget_item_fields(const std::string& name, int index,
                                            const std::string& text, const std::string& align,
                                            float cw, float ch, int icon) {
        std::lock_guard<std::mutex> lock(screenMutex);

        for (auto& w : currentScreen.widgets) {
            if (w.name != name) continue;
            if (index < 0 || index >= (int)w.items.size()) return;

            WidgetItem& item = w.items[index];
            bool changed = false;

            if (item.text != text) {
                item.text = text;
                // ⚠️ نسيج النص ملك globalTextCache: نصفّر المؤشر فقط ولا نحذفه
                item.textTexId = 0;
                item.textW = 0.0f; item.textH = 0.0f;
                item.textFirstLineW = 0.0f; item.textFirstLineH = 0.0f;
                changed = true;
            }

            if (!align.empty() && item.customAlign != align) {
                item.customAlign = align;
                changed = true;
            }

            if (cw >= 0.0f && item.customW != cw) { item.customW = cw; changed = true; }
            if (ch >= 0.0f && item.customH != ch) { item.customH = ch; changed = true; }

            if (icon >= 0 && item.iconCodepoint != (unsigned int)icon) {
                if (item.iconTexId != 0) { glDeleteTextures(1, &item.iconTexId); item.iconTexId = 0; }
                item.iconCodepoint = (unsigned int)icon;
                changed = true;
            }

            if (changed) {
                invalidate_static_scene_cache();
                invalidate_layer_scene_cache();
                w.textScrollStartTime = -1.0f;
                // 🚀 النص الجديد يحتاج نسيجاً؛ نرفع العلم ليولّده مسار الرسم
                //    في الإطار التالي بلا انتظار ضغطة زر.
                w.textPending = true;
                glm_request_render();
            }
            return;
        }
    }

    // ==================================================================================
    // 🔍 إطفاء/إشعال تكبير العنصر المحدد لويدجت معيّن — تبديل لحظي لا أكثر.
    //    مجرد قلب لنفس علم ZoomStatus؛ بلا أي تنعيم إطاري حتى لا يُحمَّل
    //    مسار الرسم شيئاً أثناء الكاروسال.
    // ==================================================================================
    void internal_set_widget_zoom(const std::string& name, int enabled) {
        std::lock_guard<std::mutex> lock(screenMutex);
        for (auto& w : currentScreen.widgets) {
            if (w.name != name) continue;
            float target = (enabled != 0) ? 1.0f : 0.0f;
            if (w.zoomShiftTo == target) return;
            // الانتقال يبدأ من القيمة المعروضة حالياً، فالعكس في منتصف الحركة سلس
            w.zoomShiftFrom  = w.zoomShift;
            w.zoomShiftTo    = target;
            w.zoomShiftStart = -1.0f;     // يُختم بزمن أول إطار قادم
            invalidate_static_scene_cache();
            invalidate_layer_scene_cache();
            glm_request_render();
            return;
        }
    }

    // ==================================================================================
    // 📊 تحديث نسبة شريط التقدّم لعنصر واحد بلا إعادة إرسال القائمة
    //    value: 0..100 = نسبة، أي قيمة سالبة = إخفاء الشريط لهذا العنصر
    // ==================================================================================
    void internal_set_widget_item_progress(const std::string& name, int index, float value) {
        std::lock_guard<std::mutex> lock(screenMutex);
        for (auto& w : currentScreen.widgets) {
            if (w.name != name) continue;
            if (index < 0 || index >= (int)w.items.size()) return;

            if (fabs(w.items[index].progress - value) < 0.05f) return;   // لا شيء تغيّر
            w.items[index].progress = value;

            invalidate_static_scene_cache();
            invalidate_layer_scene_cache();
            glm_request_render();
            return;
        }
    }

    // ==================================================================================
    // 🚀 ضبط إزاحة التمرير يدوياً (لقوائم النافذة المتحركة / Virtual Window)
    // ==================================================================================
    // بايثون يرسل نافذة صغيرة من العناصر (٤ صفوف مثلاً) بدل القائمة كلها، ثم
    // يضبط الإزاحة على الموضع البصري القديم قبل تغيير التحديد، فيتولّى المحرك
    // الانزلاق الناعم من الموضع القديم إلى الجديد كأن القائمة كاملة.
    // ==================================================================================
    void internal_set_widget_scroll(const std::string& name, float offset, int keepAnim) {
        std::lock_guard<std::mutex> lock(screenMutex);

        for (auto& w : currentScreen.widgets) {
            if (w.name != name) continue;

            if (offset < 0.0f) offset = 0.0f;
            w.currentScrollOffset = offset;
            w.startScrollOffset = offset;
            w.scrollbarLastMoveTime = -1.0f;   // 📏 إيقاظ شريط التمرير

            if (keepAnim) {
                // 🚀 نبدأ انزلاقاً من هذه الإزاحة نحو هدف التحديد الحالي، حتى لو لم
                //    يتغيّر رقم التحديد (وهو ما يحدث بعد إعادة إرسال نافذة العناصر).
                w.isAnimating = true;
                w.animStartTime = -1.0f;
                w.prevIndex = w.selectedIndex;
                w.visualIndex = (float)w.selectedIndex; w.zoomVisualIndex = (float)w.selectedIndex;;
                anyWidgetAnimating.store(true);
            } else {
                w.isAnimating = false;
                w.visualIndex = (float)w.selectedIndex; w.zoomVisualIndex = (float)w.selectedIndex;;
            }

            invalidate_static_scene_cache();
            invalidate_layer_scene_cache();
            glm_request_render();
            return;
        }
    }

    void update_widget_item_image(const char* widgetName, int index, const char* newImagePath) {
        if (!widgetName || !newImagePath) return;
        invalidate_layer_scene_cache();
        std::string wName(widgetName);
        std::string imgPath(newImagePath);
        
        for (auto& w : currentScreen.widgets) {
            if (w.name == wName) {
                if (index >= 0 && index < (int)w.items.size()) {
                    // إذا كانت الصورة الحالية هي نفسها الجديدة تماماً، لا نفعل شيء لمنع الرمشة
                    if (w.items[index].imagePath == imgPath) {
                        break;
                    }
                    
                    w.items[index].imagePath = imgPath;
                    w.items[index].loaded = false;
                    unload_widget_item_texture(w.items[index]); 
                    
                    // استدعاء نظام الذاكرة الذكي الأصلي والمدعوم في ملفك
                    manage_widget_textures(w);
                }
                break;
            }
        }
    }
    
    // ==================================================================================
    // 🚀 NEW PYTHON API WRAPPERS (Task Queue Threading)
    // ==================================================================================
    
    // يُستدعى قبل init_gles_system. القيمة تخص عملية الإقلاع التالية فقط.
    void set_seamless_startup(int enabled) {
        seamless_startup_requested.store(enabled != 0);
    }

    void init_gles_system(int width, int height) {
        // ⚠️ حارس إلزامي: إسناد std::thread إلى كائن ما زال joinable
        //    يستدعي std::terminate() فوراً = انهيار كامل لإنيجما2.
        //    كان يكفي استدعاء init مرتين بلا deinit بينهما.
        if (render_thread.joinable()) {
            GLM_LOG("[GLM] init called while engine still running - shutting down previous instance\n");
            engine_exiting = true;
            engine_running = false;
            renderCv.notify_all();
            render_thread.join();
            engine_exiting = false;
        }

        engine_screen_w = width;
        engine_screen_h = height;
        engine_running  = true;
        engine_exiting  = false;

        // exchange(false): يمنع انتقال وضع YouTube إلى أي بلوجين آخر.
        const bool seamlessStartup = seamless_startup_requested.exchange(false);
        auto now = std::chrono::high_resolution_clock::now();
        engine_start_time = std::chrono::time_point_cast<std::chrono::milliseconds>(now).time_since_epoch().count();

        render_thread = std::thread([width, height, seamlessStartup]() {
            // 🚀 أولوية قصوى لمسار الرسم — كما كانت في نسختك الأصلية.
            // خفّضتُها سابقاً خشية أن تخنق النظام، لكن سبب ذلك الخطر كان
            // fsync داخل printf وهو محذوف الآن، كما أن الحلقة صارت تنام على
            // condition_variable بدل الدوران. والأهم: عمال التحميل نزلوا إلى
            // SCHED_OTHER فلم يعودوا ينافسون هذا المسار (كانوا يرثون نفس
            // أولوية FIFO منه لأنهم يُنشأون من داخله).
            {
                struct sched_param params;
                std::memset(&params, 0, sizeof(params));
                params.sched_priority = sched_get_priority_max(SCHED_FIFO);
                int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &params);
                if (rc != 0) {
                    GLM_LOG("[GLM] SCHED_FIFO unavailable (%s) - using default scheduling\n",
                            std::strerror(rc));
                }
            }

            // الوضع العادي: reset_fb + تأخير 400ms كما كان، لحماية بقية البلوجينات.
            // الوضع السلس: لا نقفز إلى framebuffer أسود ولا نترك EGL بلا رسم 400ms.
            internal_init_gles_system(width, height, !seamlessStartup);
            if (!seamlessStartup) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }

            // 🕐 ساعة الأنيميشن — محلية للمسار وتبدأ من الصفر في كل تشغيل.
            // كانت متغيرات static تحتفظ بالزمن المتراكم عبر دورات فتح/إغلاق البلوجين،
            // فتقارَن أزمنة الأنيميشن الجديدة بساعة قديمة.
            auto  last_frame_time     = std::chrono::high_resolution_clock::now();
            float smoothed_delta      = 16666.0f;
            long long accumulated_us  = 0;
            bool  volumeOnlyMode      = false;

            while (engine_running.load()) {
                execute_gl_tasks(); // تنفيذ أوامر بايثون إن وجدت

                // 🚀 فلتر الزمن التراكمي الناعم (Low-Pass Filter)
                auto current = std::chrono::high_resolution_clock::now();
                long long delta_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(current - last_frame_time).count();
                last_frame_time = current;
                if (delta_us > 30000) delta_us = 16666;   // كبح القفزات الكبيرة
                if (delta_us < 0)     delta_us = 0;

                smoothed_delta  = (smoothed_delta * 0.85f) + (float)delta_us * 0.15f;
                accumulated_us += (long long)smoothed_delta;
                float t = (float)accumulated_us / 1000000.0f;

                // 📊 سبب رسم هذا الإطار (للتشخيص): 1=force 2=volume 3=anim 4=screen 5=textscroll
                int renderReason = 0;
                bool needs_render = force_render.exchange(false);
                if (needs_render) renderReason = 1;
                if (volume_overlay_active()) { needs_render = true; if (!renderReason) renderReason = 2; }
                if (!needs_render) {
                    if (anyWidgetAnimating.load()) {
                        needs_render = true; renderReason = 3;
                    } else {
                        std::lock_guard<std::mutex> lock(screenMutex);
                        if (currentScreen.slideActive || currentScreen.isMaskFading ||
                            popFadeDuration > 0.0f ||
                            currentScreen.screenFadeState != 0 || currentScreen.isBgFading ||
                            trailer_post_render_until.load() > t ||
                            trailer_cover_alpha.load() > 0.0f || trailer_cover_visible.load()) {
                            needs_render = true; renderReason = 4;
                        } else {
                            for (const auto& w : currentScreen.widgets) {
                                // 🐛 كان الشرط بلا فحص scrollTextMode: أي ويدجت فيها نص
                                //    تبقي textScrollActive = true إلى الأبد (تُرفع في
                                //    update_widget_items و set_widget_selection ولا تُخفض
                                //    إلا عند انتهاء دورة Marquee لا تعمل أصلاً حين
                                //    scrolltext="0"). النتيجة: المحرك يرسم المشهد كاملاً
                                //    ٦٠ مرة في الثانية بلا توقف فتثقل كل حركة.
                                if ((w.scrollTextMode != 0 && w.textScrollActive) || w.localAnimActive) {
                                    needs_render = true; renderReason = 5; break;
                                }
                            }
                        }
                    }
                }

                bool volNowActive = volume_overlay_active();

                if (engine_exiting.load()) {
                    // 🚀 وضع الإغلاق: لا نرسم أي إطار جديد إطلاقاً.
                    // أوامر بايثون المتأخرة كانت تلغي الكاش وتعيد بناء المشهد قبيل الموت = رمشة.
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                } else if (needs_render && !ui_hidden.load()) {
                    volumeOnlyMode = false;

                    // 📊 قياس زمن الإطار: نطبع سطراً واحداً فقط عند الإطار البطيء
                    //    (كل نصف ثانية كحد أقصى) لمعرفة سبب أي تقطيع بدقة.
                    g_perf_texts.store(0);
                    g_perf_items.store(0);
                    g_perf_drawn.store(0);
                    g_perf_present_us.store(0);
                    g_perf_bg_us.store(0);
                    g_perf_text_us.store(0);
                    g_perf_px.store(0);
                    auto perfStart = std::chrono::high_resolution_clock::now();

                    internal_render_frame(width, height, t);

                    double frameMs = std::chrono::duration<double, std::milli>(
                        std::chrono::high_resolution_clock::now() - perfStart).count();
                    static float lastPerfPrint = -10.0f;
                    if (frameMs > 25.0 && (t - lastPerfPrint) > 0.5f) {
                        lastPerfPrint = t;
                        double presentMs = g_perf_present_us.load() / 1000.0;
                        printf("[GLM PERF] slow %.1f ms reason=%d | bg=%.1f txt=%.1f present=%.1f other=%.1f | drawn=%d px=%lldk texts=%d\n",
                               frameMs, renderReason,
                               g_perf_bg_us.load() / 1000.0, g_perf_text_us.load() / 1000.0,
                               presentMs,
                               frameMs - (g_perf_bg_us.load() + g_perf_text_us.load() + g_perf_present_us.load()) / 1000.0,
                               g_perf_drawn.load(), (long long)(g_perf_px.load() / 1000), g_perf_texts.load());
                        fflush(stdout);
                    }
                } else if (ui_hidden.load() && (volNowActive || volumeOnlyMode)) {
                    // 🔊 الواجهة مخفية (فيديو/تريلر) لكن نريد شريط الصوت فوقه
                    internal_render_volume_only(width, height, t);
                    volumeOnlyMode = volNowActive;   // آخر إطار يمسح الشريط من الشاشة
                } else {
                    // ==========================================================
                    // 💤 لا شيء يُرسم: ننام حتى يوقظنا حدث فعلي.
                    // كان هنا sleep_for(16ms) ثابت = 60 استيقاظاً في الثانية
                    // على شاشة ساكنة تماماً. الآن ننتظر إشارة، مع مهلة قصوى
                    // 200ms كشبكة أمان لو ضاعت إشارة ما.
                    // ==========================================================
                    physics_resync.store(true);   // ⏱️ لم يُرسم إطار: الساعة ستحتاج مزامنة
                    std::unique_lock<std::mutex> lk(renderCvMutex);
                    renderCv.wait_for(lk, std::chrono::milliseconds(200), [] {
                        return !engine_running.load() || engine_exiting.load() ||
                               force_render.load() || anyWidgetAnimating.load() ||
                               volume_overlay_active();
                    });
                }
            }
            internal_deinit_gles_system(); // تدمير OpenGL قبل الخروج!
        });
    }

    void deinit_gles_system() {
        // 🚀 أولاً: أوقف الرسم فوراً (قبل engine_running)
        // حتى لا يلتقط المحرك أوامر بايثون المتبقية ويرسم إطاراً أخيراً مشوهاً.
        engine_exiting = true;
        engine_running = false;
        renderCv.notify_all();   // إيقاظ فوري بدل انتظار انتهاء المهلة
        loaderCv.notify_all();
        if (render_thread.joinable()) render_thread.join();
        engine_exiting = false; // تهيئة للفتحة القادمة
    }

    void render_frame(int w, int h, float currentTime) { glm_request_render(); }
    // 🚀 تحويل جميع الواجهات لتكون سريعة جداً (Async)
    void clear_screen() { run_on_gl_thread_async([]() { internal_clear_screen(); }); }
    void load_interface_xml(const char* xmlPath, const char* screenName, const char* pluginPath) { std::string x = xmlPath ? xmlPath : ""; std::string s = screenName ? screenName : ""; std::string p = pluginPath ? pluginPath : ""; run_on_gl_thread_async([x, s, p]() { internal_load_interface_xml(x.c_str(), s.c_str(), p.c_str()); }); }

    // ==================================================================================
    // 🪟 سكرين فوق سكرين — واجهة بايثون
    // ==================================================================================
    // push_interface_xml(xml, layoutName, pluginPath, dim, animType, animDuration, fadeIn)
    //   dim          : تعتيم الشاشة التي تحت (0.0 = بلا تعتيم، 0.5 = نصف، 1.0 = أسود)
    //   animType     : أنيميشن دخول النافذة، مثل "zoom_fade" أو "slide_up" أو "" لتعطيله
    //   animDuration : مدة الأنيميشن/الفيد بالثواني (0 = الافتراضي)
    //   fadeIn       : 1 لتفعيل ظهور تدريجي للنافذة فوق الشاشة المجمّدة
    void push_interface_xml(const char* xmlPath, const char* screenName, const char* pluginPath,
                            float dim, const char* animType, float animDuration, int fadeIn) {
        std::string x = xmlPath ? xmlPath : "";
        std::string s = screenName ? screenName : "";
        std::string p = pluginPath ? pluginPath : "";
        std::string a = animType ? animType : "";
        run_on_gl_thread_async([x, s, p, dim, a, animDuration, fadeIn]() {
            internal_push_interface_xml(x.c_str(), s.c_str(), p.c_str(), dim, a.c_str(), animDuration, fadeIn);
        });
    }

    // إغلاق النافذة العليا والعودة للشاشة التي تحتها بحالتها الكاملة.
    // fadeDuration > 0 يعطي تلاشياً ناعماً للنافذة الخارجة (مثلاً 0.25).
    void pop_interface_xml(float fadeDuration) {
        run_on_gl_thread_async([fadeDuration]() { internal_pop_interface_xml(fadeDuration); });
    }

    // العودة إلى الشاشة الجذر دفعة واحدة.
    void pop_all_screens(float fadeDuration) {
        run_on_gl_thread_async([fadeDuration]() { internal_pop_all_screens(fadeDuration); });
    }

    // عدد الطبقات الموجودة تحت الشاشة الحالية (0 = لا يوجد تراكب).
    int get_screen_depth() { return g_screen_depth.load(); }

    // 🔍 تشخيص: قناع الشاشة الفعّالة والقناع المخبوز في باكدروبها الحالي.
    //    يُطبع من بايثون للتأكد أن mask_normal سليم بعد الـ pop:
    //      print(ctypes.c_char_p(lib.get_mask_debug()).value)
    const char* get_mask_debug() {
        static std::string out;
        std::lock_guard<std::mutex> lock(screenMutex);
        out = "depth=" + std::to_string(g_screen_depth.load()) +
              " screenMask='" + currentScreen.maskPath + "'" +
              " bakedMask='"  + currentScreen.bgBakedMask + "'" +
              " bg='" + currentScreen.currentBg.imagePath + "'";
        return out.c_str();
    }

    // تغيير تعتيم الطبقة السفلية أثناء العرض.
    void set_layer_dim(float dim) {
        run_on_gl_thread_async([dim]() { internal_set_layer_dim(dim); });
    }
    void set_widget_scroll(const char* name, float offset, int keepAnim) {
        std::string n = name ? name : "";
        run_on_gl_thread_async([n, offset, keepAnim]() {
            internal_set_widget_scroll(n, offset, keepAnim);
        });
    }

    void update_widget_item_text(const char* widgetName, int index, const char* text,
                                 const char* align, float customW, float customH, int icon) {
        std::string w = widgetName ? widgetName : "";
        std::string t = text ? text : "";
        std::string a = align ? align : "";
        run_on_gl_thread_async([w, index, t, a, customW, customH, icon]() {
            internal_update_widget_item_fields(w, index, t, a, customW, customH, icon);
        });
    }

    // 🔍 إطفاء/إشعال التكبير لويدجت معيّن بحركة ناعمة (1 = مُشعَل، 0 = مُطفأ)
    void set_widget_zoom(const char* widgetName, int enabled) {
        std::string w = widgetName ? widgetName : "";
        run_on_gl_thread_async([w, enabled]() { internal_set_widget_zoom(w, enabled); });
    }

    // 📊 تحديث نسبة شريط التقدّم لعنصر واحد (سريع، بلا إعادة بناء القائمة)
    void set_widget_item_progress(const char* widgetName, int index, float value) {
        std::string w = widgetName ? widgetName : "";
        run_on_gl_thread_async([w, index, value]() {
            internal_set_widget_item_progress(w, index, value);
        });
    }

    void update_widget_items(const char* widgetName, const char* pathsString) { std::string w = widgetName ? widgetName : ""; std::string p = pathsString ? pathsString : ""; run_on_gl_thread_async([w, p]() { internal_update_widget_items(w.c_str(), p.c_str()); }); }
    void set_widget_selection(const char* widgetName, int newIndex) {
        std::string w = widgetName ? widgetName : "";
        run_on_gl_thread_async([w, newIndex]() { internal_set_widget_selection(w.c_str(), newIndex); });
    }
    void set_widget_focus(const char* name, int isFocused) { std::string n = name ? name : ""; run_on_gl_thread_async([n, isFocused]() { internal_set_widget_focus(n.c_str(), isFocused); }); }

    // 🚀 تغليف الدوال المتبقية في نظام الطابور (للقضاء على تكسر الأنيميشن والرمشة)
    void trigger_mask_fade(float x, float y, float w, float h, float duration) { run_on_gl_thread_async([x, y, w, h, duration]() { internal_trigger_mask_fade(x, y, w, h, duration); }); }
    void trigger_global_animation(const char* animType, float duration) { std::string type = animType ? animType : ""; run_on_gl_thread_async([type, duration]() { internal_trigger_global_animation(type.c_str(), duration); }); }
    void set_widget_selection_instant(const char* name, int index) { std::string n = name ? name : ""; run_on_gl_thread_async([n, index]() { internal_set_widget_selection_instant(n.c_str(), index); }); }
    void set_element_animation(const char* targetName, const char* animType) { std::string t = targetName ? targetName : ""; std::string a = animType ? animType : ""; run_on_gl_thread_async([t, a]() { internal_set_element_animation(t.c_str(), a.c_str()); }); }
    void trigger_widget_custom_anim(const char* widgetName, const char* animType, float duration, float distance) {
        std::string w = widgetName ? widgetName : ""; std::string a = animType ? animType : ""; run_on_gl_thread_async([w, a, duration, distance]() { internal_trigger_widget_custom_anim(w.c_str(), a.c_str(), duration, distance); }); 
    }
    
    // 🚀 الغلاف السحري لمنع تجميد الإنيجما عند إرسال الباكدروبات الكبيرة
    void set_background_image(const char* imgPath) { 
        std::string path = imgPath ? imgPath : ""; 
        run_on_gl_thread_async([path]() { internal_set_background_image(path.c_str()); }); 
    }

    // 🚀 بوابة التحكم بالـ Fade الشامل للشاشة من البايثون
    void set_screen_fade(int type, float duration) {
        run_on_gl_thread_async([type, duration]() { internal_set_screen_fade(type, duration); });
    }

    // 🎬 تُستدعى **قبل** load_interface_xml.
    // لا تدخل طابور المهام إطلاقاً (مجرد كتابة ذرّية)، والمحرك يطبّقها لحظة
    // اكتمال بناء الشاشة داخل نفس المهمة. بهذا يبدأ الفيد مع أول إطار للشاشة
    // الجديدة تماماً، بلا أي اعتماد على توقيت جدولة المسارات.
    //   type: 1 = ظهور من الأسود، 2 = تلاشٍ إلى الأسود، 0 = إلغاء الطلب
    void set_screen_fade_on_load(int type, float duration) {
        pending_screen_fade_type.store(type);
        if (duration > 0.0f) pending_screen_fade_duration.store(duration);
    }
    
    // 🚀 بوابة التحكم بالإخفاء من البايثون
    void set_ui_hidden(int is_hidden) { 
        run_on_gl_thread_async([is_hidden]() { 
            if (is_hidden) {
                ui_hidden = true;
                internal_clear_screen(); // 🚀 مسح شفاف فوري وبدون أي تأخير
            } else {
                ui_hidden = false;
                glm_request_render(); 
            }
        }); 
    }

    // 🚀 غطاء حماية لحظة الخروج من التريلر (Prime-style transition shield)
    void set_trailer_cover(int is_visible) {
        run_on_gl_thread_async([is_visible]() {
            if (is_visible) {
                trailer_cover_visible = true;
                trailer_cover_alpha = 1.0f;
                trailer_cover_fade_start = -1.0f;
            } else {
                trailer_cover_visible = false;
                if (trailer_cover_alpha.load() <= 0.0f) trailer_cover_alpha = 0.0f;
                trailer_cover_fade_start = -1.0f; // يبدأ fade في الفريم القادم
            }
            invalidate_static_scene_cache();
            invalidate_layer_scene_cache();
            glm_request_render();
        });
    }

    // 🚀 بوابة التحكم بإخفاء الباكدروب فقط من البايثون (لتشغيل التريلر تحت الماسك)
    void set_backdrop_hidden(int is_hidden) { 
        run_on_gl_thread_async([is_hidden]() { 
            if (is_hidden) {
                backdrop_hidden = true;
                trailer_post_render_until = -1.0f;
            } else {
                backdrop_hidden = false;
                // سيتم ضبط trailer_post_render_until عند انتهاء fade في render_frame.
            }
            // تغيير حالة التريلر يغيّر أساس المشهد بالكامل، لذلك يجب إلغاء كل لقطات FBO القديمة.
            invalidate_static_scene_cache();
            invalidate_layer_scene_cache();
            glm_request_render(); // إجبار المحرك على مسح الباكدروب فوراً
        }); 
    }

    // ==================================================================================
    // 🔊 VOLUME OSD - الواجهة البرمجية للبايثون
    // ==================================================================================

    // إظهار/تحديث شريط الصوت. value = 0..100 ، muted = 0/1
    void show_volume_bar(int value, int muted) {
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        volume_value.store(value);
        volume_muted.store(muted != 0);

        bool wasActive = volume_active.exchange(true);
        if (!wasActive) {
            volume_show_start.store(-1.0f); // يُختم في أول إطار (فيد-إن جديد)
            volume_fill_snap.store(true);   // أول ظهور بدون انزلاق من الصفر
        }
        volume_touch_time.store(-1.0f);     // إعادة تشغيل مؤقت الإخفاء

        invalidate_static_scene_cache();
        glm_request_render();
    }

    // إخفاء فوري بدون انتظار المؤقت
    void hide_volume_bar() {
        volume_active.store(false);
        volume_show_start.store(-1.0f);
        volume_touch_time.store(-1.0f);
        volume_fill_snap.store(true);
        invalidate_static_scene_cache();
        invalidate_layer_scene_cache();
        glm_request_render();
    }

    int is_volume_bar_visible() { return volume_active.load() ? 1 : 0; }

    void set_volume_bar_timeout(float seconds) {
        if (seconds > 0.1f) volume_timeout.store(seconds);
    }

    // الموقع والحجم. مرر centerX=1 لتوسيطه أفقياً (وقتها يُتجاهل x)
    void set_volume_bar_geometry(float x, float y, float w, float h, int centerX) {
        std::lock_guard<std::mutex> vlock(volumeStyleMutex);
        volumeStyle.x = x;
        volumeStyle.y = y;
        if (w > 0.0f) volumeStyle.w = w;
        if (h > 0.0f) volumeStyle.h = h;
        volumeStyle.centerX = (centerX != 0);
        volume_style_from_python.store(true);
        glm_request_render();
    }

    // الألوان بصيغة #RRGGBB أو #RRGGBBAA. مرر NULL أو "" لعدم تغيير لون معين
    void set_volume_bar_colors(const char* panelHex, const char* trackHex, const char* fillHex,
                               const char* textHex, const char* mutedHex, const char* iconHex) {
        std::lock_guard<std::mutex> vlock(volumeStyleMutex);
        if (panelHex && *panelHex) glm_parse_hex_color(panelHex, volumeStyle.panelR, volumeStyle.panelG, volumeStyle.panelB, volumeStyle.panelA);
        if (trackHex && *trackHex) glm_parse_hex_color(trackHex, volumeStyle.trackR, volumeStyle.trackG, volumeStyle.trackB, volumeStyle.trackA);
        if (fillHex  && *fillHex)  glm_parse_hex_color(fillHex,  volumeStyle.fillR,  volumeStyle.fillG,  volumeStyle.fillB,  volumeStyle.fillA);
        if (textHex  && *textHex)  glm_parse_hex_color(textHex,  volumeStyle.textR,  volumeStyle.textG,  volumeStyle.textB,  volumeStyle.textA);
        if (mutedHex && *mutedHex) glm_parse_hex_color(mutedHex, volumeStyle.muteR,  volumeStyle.muteG,  volumeStyle.muteB,  volumeStyle.muteA);
        if (iconHex  && *iconHex)  { float ia = 1.0f; glm_parse_hex_color(iconHex, volumeStyle.iconR, volumeStyle.iconG, volumeStyle.iconB, ia); }
        volume_style_from_python.store(true);
        glm_request_render();
    }

    // الخطوط والأيقونات. iconCp / mutedIconCp هي أرقام Unicode (مثلاً 0xE050)
    void set_volume_bar_fonts(const char* fontPath, int fontSize,
                              const char* iconFontPath, int iconFontSize,
                              int iconCp, int mutedIconCp, int showPercent) {
        std::lock_guard<std::mutex> vlock(volumeStyleMutex);
        if (fontPath && *fontPath) volumeStyle.fontPath = fontPath;
        if (fontSize > 0) volumeStyle.fontSize = fontSize;
        if (iconFontPath && *iconFontPath) volumeStyle.iconFontPath = iconFontPath;
        if (iconFontSize > 0) volumeStyle.iconFontSize = iconFontSize;
        if (iconCp > 0) volumeStyle.iconCp = (uint32_t)iconCp;
        if (mutedIconCp > 0) volumeStyle.iconMutedCp = (uint32_t)mutedIconCp;
        if (showPercent >= 0) volumeStyle.showPercent = showPercent;
        volume_style_from_python.store(true);
        glm_request_render();
    }

    // الانحناءات وارتفاع الشريط الداخلي
    void set_volume_bar_shape(float cornerDia, float barCornerDia, float barHeight) {
        std::lock_guard<std::mutex> vlock(volumeStyleMutex);
        if (cornerDia    >= 0.0f) volumeStyle.radius    = cornerDia / 2.0f;
        if (barCornerDia >= 0.0f) volumeStyle.barRadius = barCornerDia / 2.0f;
        if (barHeight    >= 0.0f) volumeStyle.barHeight = barHeight;
        volume_style_from_python.store(true);
        glm_request_render();
    }

    // 🌫️ تلاشي عنصر متزامن تماماً مع أنيميشنه: 0=بلا، 1=دخول، 2=خروج
    void set_element_fade(const char* targetName, int mode) {
        std::string t = targetName ? targetName : "";
        run_on_gl_thread_async([t, mode]() { internal_set_element_fade(t.c_str(), mode); });
    }

    void set_element_anim_distance(const char* targetName, float distance) { 
        std::string t = targetName ? targetName : ""; 
        run_on_gl_thread_async([t, distance]() { internal_set_element_anim_distance(t.c_str(), distance); }); 
    }

    void set_element_anim_speed(const char* targetName, float speed) { 
        std::string t = targetName ? targetName : ""; 
        run_on_gl_thread_async([t, speed]() { internal_set_element_anim_speed(t.c_str(), speed); }); 
    }

    // 🗂️ ضبط مجلد الكاش من بايثون (كان مساراً مثبتاً في الكود).
    // يُستدعى قبل init_gles_system. إن لم يكن قابلاً للكتابة يتراجع تلقائياً إلى /tmp.
    void set_cache_dir(const char* path) {
        if (!path || !*path) return;
        CACHE_DIR = path;
        if (CACHE_DIR.back() != '/') CACHE_DIR += '/';
        ensure_cache_dir();
    }

    const char* get_secret_api() {
        // يمكنك حتى هنا تقسيمه لتصعيب استخراجه ببرامج الـ Hex
        static std::string url = std::string("http://") + "dreampanel" + ".ddns.net" + "/activate.php";
        return url.c_str();
    }
}

// ==================================================================================
// 🧹 تنظيف المراجع المعلّقة لنسج النصوص
// ==================================================================================
// كاش النصوص هو المالك الوحيد لهذه النسج، والعناصر تستعير الـ ID فقط.
// عند إزاحة مدخل من الكاش (LRU) أو عند إفراغه بالكامل، يجب ألا يبقى في الواجهة
// عنصر يشير إلى نسيج محذوف — وإلا رُسم بنسيج يخص شيئاً آخر بعد إعادة استخدام الرقم.
// تصفير المرجع هنا يجعل العنصر يُعيد توليد نصه تلقائياً عند الحاجة.
//
// ملاحظة: extern "C" لا ينشئ نطاقاً، لذلك currentScreen وأنواع العناصر
//        مرئية هنا رغم أنها معرّفة داخل الكتلة أعلاه.
// ==================================================================================
static void glm_forget_text_textures(const std::vector<GLuint>& ids) {
    if (ids.empty()) return;

    // مسحة واحدة على المشهد لكل الدفعة، ببحث ثنائي بدل خطي:
    // إخلاء كامل (3072 معرّفاً) على قائمة فيها 2000 عنصر كان يعني ملايين المقارنات.
    std::vector<GLuint> sorted(ids);
    std::sort(sorted.begin(), sorted.end());

    struct Contains {
        const std::vector<GLuint>& v;
        explicit Contains(const std::vector<GLuint>& vv) : v(vv) {}
        bool operator()(GLuint id) const {
            return id != 0 && std::binary_search(v.begin(), v.end(), id);
        }
    } has(sorted);

    // 🪟 نمسح الشاشة الحيّة **وكل** الطبقات المتراكبة تحتها: أي طبقة نائمة قد تحمل
    //    معرّف نسيج أُزيح من الكاش، فتستيقظ عند pop برسم نص يخص عنصراً آخر.
    auto sweep = [&has](ScreenData& s) {
            for (size_t li = 0; li < s.labels.size(); ++li) {
                LabelElement& lbl = s.labels[li];
                if (has(lbl.textureId)) { lbl.textureId = 0; lbl.texW = 0.0f; lbl.texH = 0.0f; }
            }
            for (size_t wi = 0; wi < s.widgets.size(); ++wi) {
                WidgetElement& wdg = s.widgets[wi];
                for (size_t ii = 0; ii < wdg.items.size(); ++ii) {
                    WidgetItem& item = wdg.items[ii];
                    if (has(item.textTexId)) {
                        item.textTexId = 0;
                        item.textW = 0.0f; item.textH = 0.0f;
                        item.textFirstLineW = 0.0f; item.textFirstLineH = 0.0f;
                    }
                    if (has(item.badgeTexId)) {
                        item.badgeTexId = 0;
                        item.badgeW = 0.0f; item.badgeH = 0.0f;
                    }
                }
            }
    };

    sweep(currentScreen);
    for (size_t si = 0; si < screenStack.size(); ++si) {
        sweep(screenStack[si].data);
    }
}





