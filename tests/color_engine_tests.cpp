#include "anichar/color_engine.hpp"

#include <cmath>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

using namespace anichar;
namespace {
int checks = 0;
double maxFixtureDelta = 0.0;
void check(bool ok, const std::string& label) { ++checks; if (!ok) throw std::runtime_error(label); }
void near(double actual, double expected, double eps, const std::string& label) {
    ++checks; const double difference = std::abs(actual-expected); maxFixtureDelta = std::max(maxFixtureDelta, difference);
    if (difference>eps) throw std::runtime_error(label+": got "+std::to_string(actual)+", expected "+std::to_string(expected));
}
CachedCharacter item(std::int64_t id,std::string name,Rgb rgb,double conf=.8,double ts=1.0,std::string gender="Female",std::string hair="") {
    CachedCharacter c; c.id=id;c.name=std::move(name);c.rgb=rgb;c.lab=toLab(rgb);c.confidence=conf;c.timestamp=ts;c.gender=gender;c.listedHair=hair.empty()?std::nullopt:std::optional<std::string>(hair);return c;
}
std::vector<std::int64_t> ids(const std::vector<Match>& rows){std::vector<std::int64_t> out;for(auto& r:rows)out.push_back(r.character.id);return out;}
}
int main(){try{
    check(parseColor("##0AF")==Rgb{0,170,255},"short hex parsing");
    check(parseColor("rgb (107, 182, 217)")==Rgb{107,182,217},"RGB tuple parsing");
    check(toHex({10,171,255})=="#0AABFF","hex formatting");
    check(toHsv({107,182,217})==Hsv{199,51,85},"Python colorsys HSV fixture");
    check(toHsl({107,182,217})==Hsl{199,59,64},"Python colorsys HSL fixture");
    check(toHsv({0,255,0})==Hsv{120,100,100},"green HSV");
    check(toHsl({0,0,255})==Hsl{240,100,50},"blue HSL");

    const Lab red=toLab({255,0,0});
    near(red.l,53.240587943744899,1e-10,"Python/skimage Lab red L");
    near(red.a,80.092308225692193,1e-10,"Python/skimage Lab red a");
    near(red.b,67.202751044428695,1e-10,"Python/skimage Lab red b");
    const Lab white=toLab({255,255,255});
    near(white.a,-.0024549378621063767,1e-10,"Lab white a / skimage matrix white point");
    near(toLab({0,0,0}).l,0,1e-12,"Lab black");
    near(toLab({107,182,217}).l,70.586259687429745,1e-10,"Lab target L");
    near(deltaE(red,red),0,1e-12,"identical-color CIEDE2000");
    near(deltaE(red,red,DistanceMethod::CIE76),0,1e-12,"identical-color CIE76");
    near(deltaE({0,0,0},{3,4,0},DistanceMethod::CIE76),5,1e-12,"CIE76 3-4-5");
    near(deltaE({50,2.6772,-79.7751},{50,0,-82.7485}),2.042459680156571,1e-10,"CIEDE2000 Sharma reference pair");
    near(deltaE(toLab({255,0,0}),toLab({0,0,255})),52.88139970455684,1e-10,"CIEDE2000 RGB primaries Python fixture");
    near(deltaE(toLab({0,0,0}),toLab({255,255,255})),100.00000017602524,1e-10,"CIEDE2000 black-white Python fixture");
    near(deltaE({50,2.6772,-79.7751},{50,0,-82.7485},DistanceMethod::CIE76),4.001063283678486,1e-12,"CIE76 Python fixture");
    near(deltaE(toLab({0,0,0}),toLab({255,255,255}),DistanceMethod::CIE76),100.00000013840524,1e-12,"CIE76 black-white Python fixture");
    near(similarity(0),100,1e-12,"zero-distance similarity");
    near(similarity(50),0,1e-12,"similarity floor");

    check(baseColor({56,0,0})=="black"&&baseColor({57,0,0})=="red","value .22 boundary");
    check(baseColor({209,209,209})=="gray"&&baseColor({210,210,210})=="white","white value boundary");
    check(baseColor({255,220,220})=="white"&&baseColor({255,219,219})=="pink","saturation .14 boundary");
    check(baseColor({255,63,0})=="red"&&baseColor({255,64,0})=="orange","hue 15 boundary");
    check(baseColor({255,191,0})=="orange"&&baseColor({255,192,0})=="yellow","hue 45 boundary");
    check(baseColor({222,255,0})=="yellow"&&baseColor({221,255,0})=="green","hue 68 boundary");
    check(baseColor({0,255,191})=="green"&&baseColor({0,255,192})=="cyan","hue 165 boundary");
    check(baseColor({0,213,255})=="cyan"&&baseColor({0,212,255})=="blue","hue 190 boundary");
    check(baseColor({63,0,255})=="blue"&&baseColor({64,0,255})=="purple","hue 255 boundary");
    check(baseColor({233,0,255})=="purple"&&baseColor({234,0,255})=="pink","hue 295 boundary");
    check(baseColor({255,51,103})=="pink"&&baseColor({255,51,101})=="red","hue 345 boundary");
    check(baseColor({120,80,0})=="brown"&&baseColor({200,133,0})=="orange","brown branch");
    check(colorName({255,210,210})=="light pink","light color label");
    check(colorName({100,0,0})=="dark red","dark color label");

    check(!hairConflict({255,0,0},"scarlet hair"),"red alias");
    check(!hairConflict({255,220,0},"golden blonde"),"blonde alias");
    check(!hairConflict({255,120,0},"auburn"),"near family brown/orange");
    check(hairConflict({0,0,255},"crimson hair"),"unrelated listed hair conflict");
    check(!hairConflict({0,0,255},"description without color words"),"missing listed hair");
    check(nameMatches("Mikasa Ackerman","MIKASA"),"case-insensitive ASCII substring");
    check(nameMatches("Mikasa Ackerman","M_kasa"),"SQLite underscore wildcard");
    check(nameMatches("Mikasa Ackerman","%"),"SQLite percent wildcard");
    check(!nameMatches("Mikasa Ackerman","levi"),"nonmatching name");

    const Rgb target{255,0,0};const Lab targetLab=toLab(target);
    CachedCharacter exact=item(1,"Mikasa",target,.5,10,"Female","orange hair");
    CachedCharacter justFar=item(2,"Levi",target,.9,20,"Male");justFar.lab=Lab{targetLab.l+1,targetLab.a,targetLab.b};
    CachedCharacter low=item(3,"Eren",target,.499,30);
    CachedCharacter wrongGender=item(4,"Sasha",target,.9,40,"Unknown");
    CachedCharacter conflict=item(5,"Annie",target,.9,50,"Female","blue hair");
    CachedCharacter stale=item(6,"Historia",target,.9,60);stale.version=1;
    CachedCharacter noColor=item(7,"Armin",target,.9,70);noColor.lab.reset();
    std::vector<CachedCharacter> rows{exact,justFar,low,wrongGender,conflict,stale,noColor};
    RankOptions opts;opts.target=target;opts.maxDistance=0;opts.minConfidence=.5;opts.nameQuery="mika";
    auto result=rank(rows,opts);check(ids(result)==std::vector<std::int64_t>{1},"zero distance, max-distance, confidence, name and conflict filters");
    near(result[0].distance,0,1e-12,"ranked exact distance");near(result[0].similarity,100,1e-12,"ranked exact similarity");
    opts.maxDistance=1;opts.minConfidence=.499;opts.nameQuery="";opts.rejectHairConflicts=false;opts.girlsOnly=true;
    result=rank(rows,opts);check(ids(result)==std::vector<std::int64_t>({1,3,5}),"inclusive distance/confidence and girls-only filters");
    opts.maxDistance=0;opts.minConfidence=0;opts.girlsOnly=false;opts.rejectHairConflicts=false;opts.nameQuery="%";
    result=rank(rows,opts);check(ids(result)==std::vector<std::int64_t>({1,3,4,5}),"wildcard name query and zero-distance inclusion");

    auto tiedA=item(9,"zeta",target),tiedB=item(2,"Alpha",target),tiedC=item(8,"alpha",target);
    RankOptions defaultOptions;defaultOptions.target=target;defaultOptions.maxDistance=0;
    check(ids(rank({tiedA,tiedB,tiedC},defaultOptions))==std::vector<std::int64_t>({9,2,8}),"default option uses best-match stable ties");
    std::vector<CachedCharacter> tied{tiedA,tiedB,tiedC};opts.nameQuery="";opts.maxDistance=0;opts.sort=SortOrder::BestMatch;
    check(ids(rank(tied,opts))==std::vector<std::int64_t>({9,2,8}),"default best-match tie preserves input order");
    opts.sort=SortOrder::Name;check(ids(rank(tied,opts))==std::vector<std::int64_t>({2,8,9}),"name sort tie is stable after lowercase");
    opts.sort=SortOrder::HighestConfidence;check(ids(rank(tied,opts))==std::vector<std::int64_t>({9,2,8}),"confidence ties are stable");
    opts.sort=SortOrder::NewestAnalysis;tied[0].timestamp=tied[1].timestamp=tied[2].timestamp=5;
    check(ids(rank(tied,opts))==std::vector<std::int64_t>({9,2,8}),"timestamp ties are stable");
    std::cout<<"Passed "<<checks<<" deterministic color/ranking checks; max numeric fixture delta "<<maxFixtureDelta<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
