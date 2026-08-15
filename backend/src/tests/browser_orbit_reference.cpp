#include "compute/variants.hpp"
#include "compute/colormap.hpp"
#include "compute/color_program.hpp"
#include "compute/orbit_program.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using fsd::compute::Cx;
using fsd::compute::Variant;

Cx<double> step_runtime(Variant variant, Cx<double> z, const Cx<double>& c) {
    switch (variant) {
        case Variant::Mandelbrot: return fsd::compute::variant_step<Variant::Mandelbrot, double>(z, c);
        case Variant::Tri: return fsd::compute::variant_step<Variant::Tri, double>(z, c);
        case Variant::Boat: return fsd::compute::variant_step<Variant::Boat, double>(z, c);
        case Variant::Duck: return fsd::compute::variant_step<Variant::Duck, double>(z, c);
        case Variant::Bell: return fsd::compute::variant_step<Variant::Bell, double>(z, c);
        case Variant::Fish: return fsd::compute::variant_step<Variant::Fish, double>(z, c);
        case Variant::Vase: return fsd::compute::variant_step<Variant::Vase, double>(z, c);
        case Variant::Bird: return fsd::compute::variant_step<Variant::Bird, double>(z, c);
        case Variant::Mask: return fsd::compute::variant_step<Variant::Mask, double>(z, c);
        case Variant::Ship: return fsd::compute::variant_step<Variant::Ship, double>(z, c);
        case Variant::SinZ: return fsd::compute::variant_step<Variant::SinZ, double>(z, c);
        case Variant::CosZ: return fsd::compute::variant_step<Variant::CosZ, double>(z, c);
        case Variant::ExpZ: return fsd::compute::variant_step<Variant::ExpZ, double>(z, c);
        case Variant::SinhZ: return fsd::compute::variant_step<Variant::SinhZ, double>(z, c);
        case Variant::CoshZ: return fsd::compute::variant_step<Variant::CoshZ, double>(z, c);
        case Variant::TanZ: return fsd::compute::variant_step<Variant::TanZ, double>(z, c);
        case Variant::Custom: break;
    }
    return z;
}

struct TransitionReference {
    int iter = 0;
    double norm = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double pairwise = 0.0;
    bool escaped = false;
};

double point_distance_sq(const std::vector<double>& left, const std::vector<double>& right) {
    double result = 0.0;
    for (size_t axis = 0; axis < left.size(); ++axis) {
        const double delta = left[axis] - right[axis];
        result += delta * delta;
    }
    return result;
}

double minimum_pairwise(const std::vector<std::vector<double>>& orbit) {
    double result = std::numeric_limits<double>::infinity();
    for (size_t left = 0; left < orbit.size(); ++left) {
        for (size_t right = left + 1; right < orbit.size(); ++right) {
            const double distance = point_distance_sq(orbit[left], orbit[right]);
            if (distance < result) result = distance;
        }
    }
    return std::isfinite(result) ? std::sqrt(result) : 0.0;
}

TransitionReference map_reference(Variant variant, double re, double im, bool julia,
                                  double jr, double ji, int iterations, double bailout,
                                  int pairwise_cap) {
    Cx<double> z{julia ? re : 0.0, julia ? im : 0.0};
    const Cx<double> c{julia ? jr : re, julia ? ji : im};
    double minimum_sq = std::numeric_limits<double>::infinity();
    double maximum_sq = 0.0;
    std::vector<std::vector<double>> orbit;
    const int limit = std::min(iterations, pairwise_cap);
    TransitionReference result;
    result.iter = iterations;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        z = step_runtime(variant, z, c);
        const double norm = z.re * z.re + z.im * z.im;
        minimum_sq = std::min(minimum_sq, norm);
        maximum_sq = std::max(maximum_sq, norm);
        if (iteration < limit) orbit.push_back({z.re, z.im});
        if (!std::isfinite(norm) || norm > bailout * bailout) {
            result.iter = iteration; result.norm = norm; result.escaped = true; break;
        }
    }
    result.minimum = std::isfinite(minimum_sq) ? std::sqrt(minimum_sq) : 0.0;
    result.maximum = maximum_sq > 0.0 ? std::sqrt(maximum_sq) : 0.0;
    result.pairwise = minimum_pairwise(orbit);
    return result;
}

TransitionReference pair_reference(Variant from, Variant to, double theta, double u, double v,
                                   bool julia, double jr, double ji, int iterations,
                                   double bailout, int pairwise_cap) {
    const double cosine = std::cos(theta), sine = std::sin(theta);
    double x = u, y = v * cosine, z = v * sine;
    const double cx = julia ? jr : x;
    const double cy = julia ? ji * cosine : y;
    const double cz = julia ? ji * sine : z;
    double x2 = x*x, y2 = y*y, z2 = z*z;
    double minimum_sq = x2 + y2 + z2, maximum_sq = minimum_sq;
    std::vector<std::vector<double>> orbit{{x,y,z}};
    TransitionReference result;
    result.iter = iterations;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const double nx = fsd::compute::variant_transition_real_projection(from, x2, y2)
            + fsd::compute::variant_transition_real_projection(to, x2, z2) - x2 + cx;
        const double ny = fsd::compute::variant_transition_imag_projection(from, x, y) + cy;
        const double nz = fsd::compute::variant_transition_imag_projection(to, x, z) + cz;
        const bool finite = std::isfinite(nx) && std::isfinite(ny) && std::isfinite(nz);
        const double norm = finite ? nx*nx + ny*ny + nz*nz : std::numeric_limits<double>::infinity();
        minimum_sq = std::min(minimum_sq, norm); maximum_sq = std::max(maximum_sq, norm);
        if (static_cast<int>(orbit.size()) < pairwise_cap) orbit.push_back({nx,ny,nz});
        if (!finite || norm > bailout*bailout) {
            result.iter = iteration; result.norm = norm; result.escaped = true; break;
        }
        x=nx; y=ny; z=nz; x2=x*x; y2=y*y; z2=z*z;
    }
    result.minimum=std::sqrt(minimum_sq); result.maximum=std::sqrt(maximum_sq);
    result.pairwise=minimum_pairwise(orbit);
    return result;
}

struct MultiLegReference { Variant variant; double direction; double influence; };

TransitionReference multi_reference(const std::vector<std::pair<Variant,double>>& input,
                                    double u, double v, bool julia, double jr, double ji,
                                    int iterations, double bailout, int pairwise_cap) {
    double maximum_weight=0.0, sum_sq=0.0;
    for (const auto& leg : input) if (leg.second > 0.0) {
        maximum_weight=std::max(maximum_weight,leg.second); sum_sq+=leg.second*leg.second;
    }
    std::vector<MultiLegReference> legs;
    for (const auto& leg : input) if (leg.second > 0.0) {
        legs.push_back({leg.first,leg.second/std::sqrt(sum_sq),leg.second/maximum_weight});
    }
    double x=u, x2=x*x; const double cx=julia?jr:u;
    std::vector<double> axis, axis2, constants;
    for (const auto& leg : legs) {
        axis.push_back(v*leg.direction); axis2.push_back(axis.back()*axis.back());
        constants.push_back(julia?ji*leg.direction:axis.back());
    }
    double minimum_sq=x2; for(double value:axis2) minimum_sq+=value;
    double maximum_sq=minimum_sq;
    std::vector<std::vector<double>> orbit;
    std::vector<double> initial{x}; initial.insert(initial.end(),axis.begin(),axis.end()); orbit.push_back(initial);
    TransitionReference result; result.iter=iterations;
    for(int iteration=0;iteration<iterations;++iteration){
        double real_sum=0.0,influence_sum=0.0; std::vector<double> next(axis.size());
        for(size_t k=0;k<legs.size();++k){
            real_sum += legs[k].influence*fsd::compute::variant_transition_real_projection(legs[k].variant,x2,axis2[k]);
            influence_sum += legs[k].influence;
            next[k]=legs[k].influence*fsd::compute::variant_transition_imag_projection(legs[k].variant,x,axis[k])+constants[k];
        }
        const double nx=real_sum-(influence_sum-1.0)*x2+cx;
        double norm=nx*nx; bool finite=std::isfinite(nx);
        for(double value:next){finite=finite&&std::isfinite(value);norm+=value*value;}
        if(!finite) norm=std::numeric_limits<double>::infinity();
        minimum_sq=std::min(minimum_sq,norm); maximum_sq=std::max(maximum_sq,norm);
        if(static_cast<int>(orbit.size())<pairwise_cap){std::vector<double> point{nx};point.insert(point.end(),next.begin(),next.end());orbit.push_back(point);}
        if(!finite||norm>bailout*bailout){result.iter=iteration;result.norm=norm;result.escaped=true;break;}
        x=nx;x2=x*x;axis=next;for(size_t k=0;k<axis.size();++k)axis2[k]=axis[k]*axis[k];
    }
    result.minimum=std::sqrt(minimum_sq);result.maximum=std::sqrt(maximum_sq);result.pairwise=minimum_pairwise(orbit);
    return result;
}

void emit_transition_result(const char* kind, const char* name, double u, double v,
                            bool julia, const TransitionReference& result) {
    std::cout << kind << '\t' << name << '\t' << std::setprecision(17) << u << '\t' << v << '\t'
              << (julia?1:0) << '\t' << result.iter << '\t' << result.norm << '\t'
              << (result.escaped?1:0) << '\t' << result.minimum << '\t' << result.maximum << '\t'
              << 0.5*(result.minimum+result.maximum) << '\t' << result.pairwise << '\n';
}

template <Variant V>
void emit_sample(const char* name, double re, double im, bool julia, double jr, double ji, int max_iter) {
    Cx<double> z{julia ? re : 0.0, julia ? im : 0.0};
    const Cx<double> c{julia ? jr : re, julia ? ji : im};
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = 0.0;
    int iteration = max_iter;
    double norm = 0.0;
    std::vector<Cx<double>> pairwise_orbit;
    double pairwise_min_sq = std::numeric_limits<double>::infinity();
    const double bailout = fsd::compute::variant_default_bailout(V);
    for (int i = 0; i < max_iter; ++i) {
        z = fsd::compute::variant_step<V, double>(z, c);
        if (i < 64) {
            for (const auto& previous : pairwise_orbit) {
                const double dr = z.re - previous.re; const double di = z.im - previous.im;
                pairwise_min_sq = std::min(pairwise_min_sq, dr * dr + di * di);
            }
            pairwise_orbit.push_back(z);
        }
        norm = z.re * z.re + z.im * z.im;
        minimum = std::min(minimum, norm);
        maximum = std::max(maximum, norm);
        const bool escaped = !std::isfinite(norm) || (fsd::compute::variant_is_transcendental(V)
            ? std::max(std::abs(z.re), std::abs(z.im)) >= bailout : norm > bailout * bailout);
        if (escaped) { iteration = i; break; }
    }
    if (iteration == max_iter) norm = 0.0;
    std::cout << name << '\t' << std::setprecision(17) << re << '\t' << im << '\t'
              << (julia ? 1 : 0) << '\t' << jr << '\t' << ji << '\t' << max_iter << '\t'
              << iteration << '\t' << norm << '\t'
              << (std::isfinite(minimum) ? std::sqrt(minimum) : 0.0) << '\t'
              << (maximum > 0.0 ? std::sqrt(maximum) : 0.0) << '\t'
              << (std::isfinite(pairwise_min_sq) ? std::sqrt(pairwise_min_sq) : 0.0) << '\n';
}

template <Variant V>
void emit_variant(const char* name) {
    emit_sample<V>(name, -0.75, 0.1, false, 0, 0, 320);
    emit_sample<V>(name, 0.31, -0.27, false, 0, 0, 180);
    emit_sample<V>(name, -0.12, 0.72, true, -0.8, 0.156, 240);
}

template <Variant V>
void emit_frame(bool julia, fsd::compute::Colormap palette, bool smooth) {
    constexpr int width = 32, height = 24, max_iter = 180;
    constexpr double center_re = -0.64, center_im = 0.03, scale = 2.4;
    const double bailout = fsd::compute::variant_default_bailout(V);
    std::vector<unsigned char> rgba(static_cast<size_t>(width * height * 4), 255);
    for (int py = 0; py < height; ++py) for (int px = 0; px < width; ++px) {
        const double re = center_re + ((px + 0.5) / width - 0.5) * scale * width / height;
        const double im = center_im + (0.5 - (py + 0.5) / height) * scale;
        Cx<double> z{julia ? re : 0.0, julia ? im : 0.0};
        const Cx<double> c{julia ? -0.8 : re, julia ? 0.156 : im};
        int iteration = max_iter; double norm = 0.0;
        for (int i = 0; i < max_iter; ++i) {
            z = fsd::compute::variant_step<V, double>(z, c); norm = z.re*z.re + z.im*z.im;
            const bool escaped = !std::isfinite(norm) || (fsd::compute::variant_is_transcendental(V)
                ? std::max(std::abs(z.re), std::abs(z.im)) >= bailout : norm > bailout*bailout);
            if (escaped) { iteration = i; break; }
        }
        const double stored_norm = iteration < max_iter ? static_cast<float>(norm) : 0.0;
        unsigned char b=0,g=0,r=0; fsd::compute::colorize_escape_bgr(iteration, max_iter, palette, stored_norm, smooth, b, g, r);
        const size_t offset = static_cast<size_t>((py * width + px) * 4); rgba[offset]=r; rgba[offset+1]=g; rgba[offset+2]=b;
    }
    std::cout.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
}

template <Variant V>
void emit_agreement_frame() {
    constexpr int width=32,height=24,max_iter=120;
    constexpr double center_re=-.64,center_im=.03,scale=2.4;
    std::vector<unsigned char> rgba(static_cast<size_t>(width*height*4),255);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        const double re=center_re+((x+.5)/width-.5)*scale*width/height;
        const double im=center_im+(.5-(y+.5)/height)*scale;
        Cx<double> z{0,0};const Cx<double> c{re,im};bool diverged=false;int iteration=0;
        for(;iteration<max_iter;++iteration){
            const auto next=fsd::compute::variant_step<V,double>(z,c);
            if(!diverged){const auto mandel=fsd::compute::variant_step<Variant::Mandelbrot,double>(z,c);
                const double tolerance=1e-11*(1+std::abs(mandel.re)+std::abs(mandel.im));
                if(std::abs(next.re-mandel.re)>tolerance||std::abs(next.im-mandel.im)>tolerance)diverged=true;}
            z=next;if(z.re*z.re+z.im*z.im>4)break;
        }
        std::uint8_t b=0,g=0,r=0;fsd::compute::colorize_escape_bgr(iteration,max_iter,fsd::compute::Colormap::Twilight,0,false,b,g,r);
        const size_t offset=static_cast<size_t>((y*width+x)*4);
        rgba[offset]=diverged?255-r:r;rgba[offset+1]=diverged?255-g:g;rgba[offset+2]=diverged?255-b:b;
    }
    std::cout.write(reinterpret_cast<const char*>(rgba.data()),static_cast<std::streamsize>(rgba.size()));
}

void emit_program_reference() {
    using fsd::compute::OrbitParameter;
    using fsd::compute::OrbitProgram;
    using fsd::compute::OrbitSequenceStep;
    const auto polynomial = OrbitProgram::formula("z^2 + alpha*c + i*beta", {
        {"alpha", {0.75, 0.0}, OrbitParameter::Type::Real},
        {"beta", {0.10, 0.0}, OrbitParameter::Type::Real},
    });
    const auto analytic = OrbitProgram::formula("sin(z)+conj(c)/3");
    const auto sequence = OrbitProgram::sequence({
        OrbitSequenceStep{2, OrbitProgram::builtin(Variant::Mandelbrot)},
        OrbitSequenceStep{1, OrbitProgram::builtin(Variant::Boat)},
    });
    struct ProgramCase { const char* name; std::shared_ptr<const OrbitProgram> program; };
    const ProgramCase cases[] = {
        {"polynomial", polynomial}, {"analytic", analytic}, {"sequence", sequence},
    };
    for (const auto& entry : cases) {
        Cx<double> z{-0.12, 0.72}; const Cx<double> c{-0.8, 0.156};
        for (int iteration = 0; iteration < 12; ++iteration) {
            z = entry.program->step(z, c, iteration);
            std::cout << entry.name << '\t' << iteration << '\t' << std::setprecision(17)
                      << z.re << '\t' << z.im << '\n';
        }
    }
}

void emit_program_function_reference() {
    using fsd::compute::OrbitProgram;
    const std::vector<std::pair<std::string, std::string>> formulas = {
        {"sin", "sin(z)"}, {"cos", "cos(z)"}, {"tan", "tan(z)"},
        {"exp", "exp(z)"}, {"log", "log(z)"}, {"sqrt", "sqrt(z)"},
        {"abs", "abs(z)"}, {"conj", "conj(z)"}, {"sinh", "sinh(z)"},
        {"cosh", "cosh(z)"}, {"tanh", "tanh(z)"}, {"real", "real(z)"},
        {"imag", "imag(z)"}, {"pow", "pow(z,2.5)"},
        {"operators", "-z^2/3+2*z-c"},
    };
    const Cx<double> z{0.31, -0.27}; const Cx<double> c{-0.8, 0.156};
    for (const auto& [name, source] : formulas) {
        const auto program = OrbitProgram::formula(source);
        const auto result = program->step(z, c, 0);
        std::cout << name << '\t' << source << '\t' << std::setprecision(17)
                  << result.re << '\t' << result.im << '\n';
    }
}

void emit_transition_reference() {
    constexpr double pi = 3.14159265358979323846264338327950288;
    struct PairCase { const char* name; Variant from; Variant to; int angle; double u; double v; bool julia; };
    const PairCase cases[] = {
        {"negative_135",Variant::Mandelbrot,Variant::Boat,-135000,-0.31,0.42,false},
        {"negative_45",Variant::Bell,Variant::Vase,-45000,0.17,-0.63,true},
        {"positive_37",Variant::Bird,Variant::Mask,37000,-0.72,0.09,false},
        {"positive_135",Variant::Ship,Variant::Tri,135000,0.28,0.51,true},
    };
    for(const auto& entry:cases){
        const auto result=pair_reference(entry.from,entry.to,entry.angle*pi/180000.0,
            entry.u,entry.v,entry.julia,-0.8,0.156,90,2.0,11);
        emit_transition_result("pair",entry.name,entry.u,entry.v,entry.julia,result);
    }
    struct CardinalCase { const char* name; Variant variant; int angle; double u; double v; bool julia; };
    const CardinalCase cardinals[] = {
        {"zero",Variant::Mandelbrot,0,-0.55,0.26,false},
        {"positive_90",Variant::Boat,90000,-0.2,-0.48,true},
        {"negative_90",Variant::Boat,-90000,-0.2,0.48,true},
        {"positive_180",Variant::Bell,180000,0.14,-0.37,false},
        {"negative_180",Variant::Bell,-180000,0.14,-0.37,false},
    };
    for(const auto& entry:cardinals){
        const bool flip=entry.angle==-90000||std::abs(entry.angle)==180000;
        const auto result=map_reference(entry.variant,entry.u,flip?-entry.v:entry.v,
            entry.julia,-0.8,flip?-.156:.156,90,2.0,11);
        emit_transition_result("cardinal",entry.name,entry.u,entry.v,entry.julia,result);
    }
    const auto two=multi_reference({{Variant::Mandelbrot,1.0},{Variant::Boat,2.0}},-.41,.33,false,-.8,.156,90,2.0,11);
    emit_transition_result("multi","two_legs",-.41,.33,false,two);
    const auto three=multi_reference({{Variant::Bell,.5},{Variant::Fish,1.7},{Variant::Mask,2.8}},.19,-.52,true,-.8,.156,90,2.0,11);
    emit_transition_result("multi","three_legs",.19,-.52,true,three);
    const auto skip=multi_reference({{Variant::Tri,0.0},{Variant::Duck,3.0},{Variant::Ship,-2.0},{Variant::Bird,1.0}},-.67,.14,false,-.8,.156,90,2.0,11);
    emit_transition_result("multi","skip_nonpositive",-.67,.14,false,skip);

    const long long angles[] = {-900000,-540000,-450000,-360000,-270000,-180000,-90000,0,90000,180000,270000,360000,450000,540000,900000};
    for(const long long input:angles){
        long long wrapped=(input+180000)%360000;if(wrapped<0)wrapped+=360000;wrapped-=180000;
        if(wrapped==-180000&&input>0)wrapped=180000;
        std::cout<<"theta\t"<<input<<'\t'<<wrapped<<'\n';
    }
}

void emit_agreement_reference() {
    const std::array<Variant,16> variants={Variant::Mandelbrot,Variant::Tri,Variant::Boat,Variant::Duck,
        Variant::Bell,Variant::Fish,Variant::Vase,Variant::Bird,Variant::Mask,Variant::Ship,
        Variant::SinZ,Variant::CosZ,Variant::ExpZ,Variant::SinhZ,Variant::CoshZ,Variant::TanZ};
    for(Variant variant:variants){
        for(int sample=0;sample<2;++sample){
            const bool julia=sample==1; const double re=sample==0?-.72:.23; const double im=sample==0?.16:-.41;
            Cx<double> z{julia?re:0.0,julia?im:0.0}; const Cx<double> c{julia?-.8:re,julia?.156:im};
            bool diverged=false;int iteration=0;
            for(;iteration<100;++iteration){
                const Cx<double> next=step_runtime(variant,z,c);
                if(!diverged){const Cx<double> mandel=step_runtime(Variant::Mandelbrot,z,c);
                    const double tolerance=1e-11*(1.0+std::abs(mandel.re)+std::abs(mandel.im));
                    if(std::abs(next.re-mandel.re)>tolerance||std::abs(next.im-mandel.im)>tolerance)diverged=true;}
                z=next;if(z.re*z.re+z.im*z.im>4.0)break;
            }
            std::cout<<fsd::compute::variant_name(variant)<<'\t'<<std::setprecision(17)<<re<<'\t'<<im<<'\t'
                     <<(julia?1:0)<<'\t'<<iteration<<'\t'<<(!diverged?1:0)<<'\n';
        }
    }
}

void emit_color_program_reference() {
    using fsd::compute::ColorProgram; using fsd::compute::ColorWrap;
    using fsd::compute::ProgramColor; using fsd::compute::ProgramColorStop;
    const std::vector<ProgramColorStop> stops={{{0.0},{4,8,16}},{{0.27},{51,102,204}},{{.73},{240,80,35}},{{1.0},{250,245,220}}};
    const struct { const char* name; ColorWrap wrap; } wraps[]={{"clamp",ColorWrap::Clamp},{"repeat",ColorWrap::Repeat},{"mirror",ColorWrap::Mirror}};
    const double raw_values[]={-1.0,0.0,.1,.27,.5,.73,.95,1.0,2.0};
    for(const auto& entry:wraps){
        ColorProgram program(stops,entry.wrap,2.5,-.25,{7,11,19},{255,0,255});
        for(double raw:raw_values){double input=raw<=0?0:std::min(1.0,raw);std::uint8_t b=0,g=0,r=0;program.colorize(input,b,g,r);
            std::cout<<entry.name<<'\t'<<std::setprecision(17)<<raw<<'\t'<<static_cast<int>(r)<<'\t'<<static_cast<int>(g)<<'\t'<<static_cast<int>(b)<<'\n';}
        std::uint8_t b=0,g=0,r=0;program.colorizeInterior(b,g,r);std::cout<<entry.name<<"\tinterior\t"<<static_cast<int>(r)<<'\t'<<static_cast<int>(g)<<'\t'<<static_cast<int>(b)<<'\n';
        program.colorizeInvalid(b,g,r);std::cout<<entry.name<<"\tinvalid\t"<<static_cast<int>(r)<<'\t'<<static_cast<int>(g)<<'\t'<<static_cast<int>(b)<<'\n';
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--program") {
        emit_program_reference();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--program-functions") {
        emit_program_function_reference();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--transition") { emit_transition_reference(); return 0; }
    if (argc == 2 && std::string(argv[1]) == "--agreement") { emit_agreement_reference(); return 0; }
    if (argc == 2 && std::string(argv[1]) == "--color-program") { emit_color_program_reference(); return 0; }
    if (argc == 3 && std::string(argv[1]) == "--frame") {
        if (std::string(argv[2]) == "mandelbrot") emit_frame<Variant::Mandelbrot>(false, fsd::compute::Colormap::ClassicCos, false);
        else if (std::string(argv[2]) == "burning_ship_julia") emit_frame<Variant::Boat>(true, fsd::compute::Colormap::Viridis, true);
        else if (std::string(argv[2]) == "burning_ship_agree") emit_agreement_frame<Variant::Boat>();
        else return 2;
        return 0;
    }
    emit_variant<Variant::Mandelbrot>("mandelbrot");
    emit_variant<Variant::Tri>("tricorn");
    emit_variant<Variant::Boat>("burning_ship");
    emit_variant<Variant::Duck>("celtic");
    emit_variant<Variant::Bell>("heart");
    emit_variant<Variant::Fish>("buffalo");
    emit_variant<Variant::Vase>("perp_buffalo");
    emit_variant<Variant::Bird>("celtic_ship");
    emit_variant<Variant::Mask>("mandelceltic");
    emit_variant<Variant::Ship>("perp_ship");
    emit_variant<Variant::SinZ>("sin_z");
    emit_variant<Variant::CosZ>("cos_z");
    emit_variant<Variant::ExpZ>("exp_z");
    emit_variant<Variant::SinhZ>("sinh_z");
    emit_variant<Variant::CoshZ>("cosh_z");
    emit_variant<Variant::TanZ>("tan_z");
}
