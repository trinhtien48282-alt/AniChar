#include "anichar/color_engine.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <QString>

namespace anichar {
namespace {
constexpr double pi = 3.141592653589793238462643383279502884;

int pythonRound(double x) {
    const double lower = std::floor(x);
    const double fraction = x - lower;
    if (fraction < 0.5) return static_cast<int>(lower);
    if (fraction > 0.5) return static_cast<int>(lower + 1.0);
    const auto n = static_cast<long long>(lower);
    return static_cast<int>((n % 2 == 0) ? n : n + 1);
}

struct HsvFloat { double h, s, v; };
HsvFloat hsvFloat(const Rgb& rgb) {
    const double r = rgb[0] / 255.0, g = rgb[1] / 255.0, b = rgb[2] / 255.0;
    const double hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
    double h = 0.0, s = hi == 0.0 ? 0.0 : d / hi;
    if (d != 0.0) {
        if (hi == r) h = (g - b) / d;
        else if (hi == g) h = 2.0 + (b - r) / d;
        else h = 4.0 + (r - g) / d;
        h /= 6.0;
        if (h < 0.0) h += 1.0;
    }
    return {h, s, hi};
}

std::string lowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return s;
}
std::uint32_t nextCodepoint(const std::string& s, std::size_t& i) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) return c;
    unsigned n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    std::uint32_t cp = n == 3 ? c & 0x07 : n == 2 ? c & 0x0F : n == 1 ? c & 0x1F : 0xFFFD;
    for (unsigned j = 0; j < n && i < s.size(); ++j) cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    return cp;
}
std::vector<std::uint32_t> codepoints(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) out.push_back(nextCodepoint(s, i));
    return out;
}
bool sqliteLike(const std::string& name, const std::string& query) {
    auto text = codepoints(lowerAscii(name));
    auto pattern = codepoints(lowerAscii("%" + query + "%"));
    std::vector<bool> prev(text.size() + 1), next(text.size() + 1);
    prev[0] = true;
    for (auto p : pattern) {
        std::fill(next.begin(), next.end(), false);
        if (p == '%') {
            next[0] = prev[0];
            for (std::size_t i = 1; i <= text.size(); ++i) next[i] = prev[i] || next[i - 1];
        } else {
            for (std::size_t i = 0; i < text.size(); ++i)
                if (prev[i] && (p == '_' || text[i] == p)) next[i + 1] = true;
        }
        prev.swap(next);
    }
    return prev.back();
}
std::string lowerForSort(const std::string& s) {
    const auto q = QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())).toLower().toUtf8();
    return std::string(q.constData(), static_cast<std::size_t>(q.size()));
}
std::string familyToken(const std::string& word) {
    const std::string w = lowerAscii(word);
    if (w == "black" || w == "white" || w == "gray" || w == "grey" || w == "silver") return w == "grey" || w == "silver" ? "gray" : w;
    if (w == "red" || w == "crimson" || w == "scarlet") return "red";
    if (w == "orange") return "orange";
    if (w == "yellow" || w == "blonde" || w == "blond" || w == "golden") return "yellow";
    if (w == "brown" || w == "chestnut" || w == "auburn") return "brown";
    if (w == "green") return "green";
    if (w == "teal" || w == "aqua" || w == "turquoise" || w == "cyan") return "cyan";
    if (w == "blue" || w == "navy") return "blue";
    if (w == "purple" || w == "violet" || w == "lavender") return "purple";
    if (w == "pink" || w == "magenta") return "pink";
    return {};
}
std::vector<std::string> hairFamilies(const std::string& text) {
    std::vector<std::string> out; std::string word;
    auto flush = [&] { if (!word.empty()) { auto f = familyToken(word); if (!f.empty() && std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f); word.clear(); } };
    const auto lowered = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())).toLower();
    for (QChar ch : lowered) {
        const auto c = ch.unicode();
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) word.push_back(static_cast<char>(c));
        else flush();
    }
    flush(); return out;
}
bool nearFamilies(const std::string& a, const std::string& b) {
    if (a == b) return true;
    const std::pair<const char*, const char*> pairs[] = {
        {"white","gray"},{"black","gray"},{"black","brown"},{"brown","orange"},
        {"orange","yellow"},{"orange","red"},{"red","pink"},{"pink","purple"},
        {"purple","blue"},{"blue","cyan"},{"cyan","green"},{"brown","yellow"},
        {"brown","red"},{"white","yellow"},{"white","pink"}};
    return std::any_of(std::begin(pairs), std::end(pairs), [&](const auto& p) {
        return (a == p.first && b == p.second) || (a == p.second && b == p.first);
    });
}
} // namespace

Rgb parseColor(const std::string& input) {
    std::string t = lowerAscii(input);
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!t.empty() && isSpace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
    while (!t.empty() && isSpace(static_cast<unsigned char>(t.back()))) t.pop_back();
    if (t.compare(0, 3, "rgb") == 0) t.erase(0, 3);
    while (!t.empty() && (t.front() == '(' || t.front() == ')' || t.front() == ' ')) t.erase(t.begin());
    while (!t.empty() && (t.back() == '(' || t.back() == ')' || t.back() == ' ')) t.pop_back();
    if (t.find(',') != std::string::npos) {
        std::array<int, 3> values{}; std::size_t start = 0;
        for (int i = 0; i < 3; ++i) {
            const auto end = t.find(',', start);
            if ((i < 2 && end == std::string::npos) || (i == 2 && end != std::string::npos)) throw std::invalid_argument("RGB needs three whole numbers from 0 to 255");
            std::string part = t.substr(start, end == std::string::npos ? end : end - start);
            const auto first = part.find_first_not_of(" \t\r\n"), last = part.find_last_not_of(" \t\r\n");
            part = first == std::string::npos ? "" : part.substr(first, last - first + 1);
            if (part.empty() || (part[0] == '+' ) || (part[0] == '-' && part.size() == 1) || !std::all_of(part.begin() + (part[0] == '-' ? 1 : 0), part.end(), [](unsigned char c){return c >= '0' && c <= '9';})) throw std::invalid_argument("RGB needs three whole numbers from 0 to 255");
            try { values[i] = std::stoi(part); } catch (...) { throw std::invalid_argument("RGB needs three whole numbers from 0 to 255"); }
            start = end == std::string::npos ? t.size() : end + 1;
        }
        if (std::any_of(values.begin(), values.end(), [](int v){return v < 0 || v > 255;})) throw std::invalid_argument("RGB needs three whole numbers from 0 to 255");
        return values;
    }
    while (!t.empty() && t.front() == '#') t.erase(t.begin());
    if (t.size() == 3) { std::string expanded; for(char c:t){expanded.push_back(c);expanded.push_back(c);} t=expanded; }
    if (t.size() != 6 || !std::all_of(t.begin(), t.end(), [](unsigned char c){return std::isxdigit(c)!=0;})) throw std::invalid_argument("Use #RRGGBB, #RGB or 'r, g, b'");
    Rgb rgb{}; for(int i=0;i<3;++i) rgb[i]=std::stoi(t.substr(2*i,2),nullptr,16); return rgb;
}
std::string toHex(const Rgb& rgb) {
    constexpr char digits[]="0123456789ABCDEF"; std::string out="#000000";
    for(int i=0;i<3;++i){const int v=rgb[i];out[1+2*i]=digits[(v>>4)&15];out[2+2*i]=digits[v&15];} return out;
}
Hsv toHsv(const Rgb& rgb) { auto h=hsvFloat(rgb); return {pythonRound(h.h*360),pythonRound(h.s*100),pythonRound(h.v*100)}; }
Hsl toHsl(const Rgb& rgb) {
    auto h=hsvFloat(rgb); const double lo=std::min({rgb[0],rgb[1],rgb[2]})/255.0;
    const double l=(h.v+lo)/2.0, d=h.v-lo;
    const double s=d==0?0:(l<=.5?d/(h.v+lo):d/(2-h.v-lo));
    return {pythonRound(h.h*360),pythonRound(s*100),pythonRound(l*100)};
}
Lab toLab(const Rgb& rgb) {
    const double c[3]={rgb[0]/255.0,rgb[1]/255.0,rgb[2]/255.0}; double lin[3];
    for(int i=0;i<3;++i) lin[i]=c[i]>.04045?std::pow((c[i]+.055)/1.055,2.4):c[i]/12.92;
    constexpr double m[3][3]={{.412453,.357580,.180423},{.212671,.715160,.072169},{.019334,.119193,.950227}};
    double xyz[3]{}; for(int i=0;i<3;++i) for(int j=0;j<3;++j) xyz[i]+=m[i][j]*lin[j];
    xyz[0]/=.95047; xyz[2]/=1.08883;
    for(double& v:xyz) v=v>.008856?std::cbrt(v):7.787*v+16.0/116.0;
    return {116*xyz[1]-16,500*(xyz[0]-xyz[1]),200*(xyz[1]-xyz[2])};
}
double deltaE(const Lab& x,const Lab& y,DistanceMethod method) {
    if(method==DistanceMethod::CIE76) return std::hypot(std::hypot(x.l-y.l,x.a-y.a),x.b-y.b);
    const double L1=x.l,a1=x.a,b1=x.b,L2=y.l,a2=y.a,b2=y.b;
    const double Cbar=(std::hypot(a1,b1)+std::hypot(a2,b2))/2;
    const double c7=std::pow(Cbar,7), G=.5*(1-std::sqrt(c7/(c7+std::pow(25.0,7))));
    const double scale=1+G;
    const double C1=std::hypot(a1*scale,b1), C2=std::hypot(a2*scale,b2);
    auto hue=[](double a,double b){double h=std::atan2(b,a);return h<0?h+2*pi:h;};
    const double h1=hue(a1*scale,b1),h2=hue(a2*scale,b2);
    const double Lbar=(L1+L2)/2,tmp=(Lbar-50)*(Lbar-50);
    const double SL=1+.015*tmp/std::sqrt(20+tmp), Lt=(L2-L1)/SL;
    const double Cmean=(C1+C2)/2, SC=1+.045*Cmean, Ct=(C2-C1)/SC;
    const double hd=h2-h1,hs=h1+h2,cc=C1*C2;
    double dh=hd; if(hd>pi)dh-=2*pi;else if(hd< -pi)dh+=2*pi;if(cc==0)dh=0;
    const double dht=2*std::sqrt(cc)*std::sin(dh/2);
    double hb=hs;if(cc!=0&&std::abs(hd)>pi)hb+=(hs<2*pi?2*pi:-2*pi);if(cc==0)hb*=2;hb*=.5;
    const double T=1-.17*std::cos(hb-pi/6)+.24*std::cos(2*hb)+.32*std::cos(3*hb+pi/30)-.20*std::cos(4*hb-7*pi/20);
    const double SH=1+.015*Cmean*T,Ht=dht/SH;
    const double Cb7=std::pow(Cmean,7),Rc=2*std::sqrt(Cb7/(Cb7+std::pow(25.0,7)));
    const double dt=pi/6*std::exp(-std::pow((hb*180/pi-275)/25,2));
    const double Rt=-std::sin(2*dt)*Rc*Ct*Ht;
    return std::sqrt(std::max(0.0,Lt*Lt+Ct*Ct+Ht*Ht+Rt));
}
double similarity(double de,double scale) { return 100.0*std::max(0.0,1.0-de/scale); }
std::string baseColor(const Rgb& rgb) {
    const auto h=hsvFloat(rgb);const double hue=h.h*360;
    if(h.v<.22)return "black";if(h.s<.14)return h.v>.82?"white":"gray";
    if(hue<15||hue>=345)return h.s<.55&&h.v>.7?"pink":"red";
    if(hue<45)return h.v<.6&&h.s>.3?"brown":"orange";
    if(hue<68)return "yellow";if(hue<165)return "green";if(hue<190)return "cyan";
    if(hue<255)return "blue";if(hue<295)return "purple";return "pink";
}
std::string colorName(const Rgb& rgb) {
    const auto base=baseColor(rgb);const auto h=hsvFloat(rgb);
    if(base!="black"&&base!="white"&&base!="gray"){if(h.v>.85&&h.s<.5)return "light "+base;if(h.v<.4)return "dark "+base;}return base;
}
bool hairConflict(const Rgb& rgb,const std::string& listed) {
    auto fams=hairFamilies(listed);const auto seen=baseColor(rgb);
    return !fams.empty()&&!std::any_of(fams.begin(),fams.end(),[&](const auto& f){return nearFamilies(f,seen);});
}
bool nameMatches(const std::string& name,const std::string& query) { return sqliteLike(name,query); }
std::vector<Match> rank(const std::vector<CachedCharacter>& rows,const RankOptions& o) {
    std::vector<Match> out;const Lab target=toLab(o.target);
    for(const auto& row:rows){
        if(row.version!=o.currentVersion||!row.rgb||!row.lab||!row.confidence||*row.confidence<o.minConfidence)continue;
        if(!nameMatches(row.name,o.nameQuery))continue;if(o.girlsOnly&&(!row.gender||*row.gender!="Female"))continue;
        if(o.rejectHairConflicts&&row.listedHair&&!row.listedHair->empty()&&hairConflict(o.target,*row.listedHair))continue;
        const double d=deltaE(*row.lab,target,o.distance);if(d>o.maxDistance)continue;
        out.push_back({row,d,similarity(d)});
    }
    std::stable_sort(out.begin(),out.end(),[&](const Match& a,const Match& b){
        switch(o.sort){
        case SortOrder::HighestConfidence:return *a.character.confidence>*b.character.confidence;
        case SortOrder::Name:return lowerForSort(a.character.name)<lowerForSort(b.character.name);
        case SortOrder::NewestAnalysis:return a.character.timestamp>b.character.timestamp;
        case SortOrder::BestMatch:default:return a.distance<b.distance;
        }
    });return out;
}
} // namespace anichar
