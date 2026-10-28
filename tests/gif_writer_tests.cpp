#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "image/gif_writer.h"
#include "core/recording_format.h"

namespace {
using Microsoft::WRL::ComPtr;
int checks = 0, failures = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}
bool Open(srl::GifWriter& writer, const std::filesystem::path& path,
          srl::Size source, const RECT& crop, const srl::OutputSize& output, std::wstring* error) {
    ComPtr<IStream> stream;
    const HRESULT reserved=::SHCreateStreamOnFileEx(path.c_str(),
        STGM_READWRITE|STGM_SHARE_EXCLUSIVE|STGM_FAILIFTHERE,FILE_ATTRIBUTE_NORMAL,
        TRUE,nullptr,stream.GetAddressOf());
    return SUCCEEDED(reserved) && writer.Open(stream.Get(),source,crop,output,error);
}
std::vector<std::uint8_t> Solid(int width, int height, std::array<std::uint8_t, 3> bgr) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width)*height*4);
    for (std::size_t i=0; i<pixels.size(); i+=4) {
        for (std::size_t c=0;c<3;++c) pixels[i+c]=bgr[c];
        pixels[i+3]=255;
    }
    return pixels;
}
std::uint16_t Delay(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> metadata;
    PROPVARIANT value{};
    const HRESULT hr=frame->GetMetadataQueryReader(metadata.GetAddressOf());
    const HRESULT read=SUCCEEDED(hr)?metadata->GetMetadataByName(L"/grctlext/Delay", &value):hr;
    Check(SUCCEEDED(read) && value.vt==VT_UI2,"every frame has a centisecond delay");
    const auto delay=value.vt==VT_UI2?value.uiVal:USHORT{0};
    ::PropVariantClear(&value);
    return delay;
}
void CheckAnimation(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    const HRESULT initialized=::CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf()));
    Check(SUCCEEDED(initialized),"WIC image factory is available");
    if (FAILED(initialized)) return;
    ComPtr<IWICBitmapDecoder> decoder;
    const HRESULT opened=factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,decoder.GetAddressOf());
    Check(SUCCEEDED(opened),"WIC decodes the completed GIF");
    if (FAILED(opened)) return;
    UINT count=0;
    decoder->GetFrameCount(&count);
    Check(count==3,"streaming encoder preserves three animation frames");
    const std::array<USHORT,3> delays{4,7,9};
    const std::array<std::array<std::uint8_t,3>,3> colors{{{0,0,255},{0,255,0},{255,0,0}}};
    for (UINT i=0;i<count && i<3;++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        decoder->GetFrame(i,frame.GetAddressOf());
        UINT width=0,height=0;
        frame->GetSize(&width,&height);
        Check(width==5 && height==3,"GIF keeps odd requested output dimensions");
        Check(Delay(frame.Get())==delays[i],"actual capture-relative gaps determine frame delay");
        ComPtr<IWICFormatConverter> converter;
        factory->CreateFormatConverter(converter.GetAddressOf());
        HRESULT hr=converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
        std::vector<std::uint8_t> pixels(width*height*4);
        if (SUCCEEDED(hr)) hr=converter->CopyPixels(nullptr,width*4,static_cast<UINT>(pixels.size()),pixels.data());
        Check(SUCCEEDED(hr),"adaptive indexed frame converts back to BGRA");
        if (SUCCEEDED(hr)) {
            bool colorMatches=true;
            for (std::size_t pixel=0;pixel<pixels.size();pixel+=4) {
                colorMatches &= pixels[pixel]==colors[i][0] && pixels[pixel+1]==colors[i][1] &&
                    pixels[pixel+2]==colors[i][2] && pixels[pixel+3]==255;
            }
            Check(colorMatches,"crop/scale and each local palette retain all opaque source pixels");
        }
        ComPtr<IWICMetadataQueryReader> metadata;
        frame->GetMetadataQueryReader(metadata.GetAddressOf());
        PROPVARIANT disposal{};
        const HRESULT disposed=metadata->GetMetadataByName(L"/grctlext/Disposal", &disposal);
        Check(SUCCEEDED(disposed) && disposal.vt==VT_UI1 && disposal.bVal==1,
              "full opaque frames use keep disposal without trails");
        ::PropVariantClear(&disposal);
    }
    ComPtr<IWICMetadataQueryReader> metadata;
    decoder->GetMetadataQueryReader(metadata.GetAddressOf());
    PROPVARIANT application{},data{};
    const HRESULT app=metadata->GetMetadataByName(L"/appext/Application",&application);
    const HRESULT loop=metadata->GetMetadataByName(L"/appext/Data",&data);
    Check(SUCCEEDED(app) && application.vt==(VT_UI1|VT_VECTOR) && application.caub.cElems==11 &&
          std::string(reinterpret_cast<char*>(application.caub.pElems),11)=="NETSCAPE2.0",
          "GIF has the standard animation application extension");
    Check(SUCCEEDED(loop) && data.vt==(VT_UI1|VT_VECTOR) && data.caub.cElems>=4 &&
          data.caub.pElems[0]==3 && data.caub.pElems[1]==1 && data.caub.pElems[2]==0 &&
          data.caub.pElems[3]==0,"GIF loops forever");
    ::PropVariantClear(&application); ::PropVariantClear(&data);
}
void CheckTimingAndPadding(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    ::CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf()));
    ComPtr<IWICBitmapDecoder> decoder;
    const HRESULT opened=factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,decoder.GetAddressOf());
    Check(SUCCEEDED(opened),"long-hold GIF is readable");
    if (FAILED(opened)) return;
    UINT count=0; decoder->GetFrameCount(&count);
    Check(count==3,"short transients are coalesced and long holds split into representable delays");
    const std::array<USHORT,3> expected{4,65534,2};
    for (UINT i=0;i<count && i<expected.size();++i) {
        ComPtr<IWICBitmapFrameDecode> frame; decoder->GetFrame(i,frame.GetAddressOf());
        Check(Delay(frame.Get())==expected[i],"centisecond timing survives coalescing and a delay overflow");
        if (i>0) continue;
        ComPtr<IWICFormatConverter> converter; factory->CreateFormatConverter(converter.GetAddressOf());
        HRESULT hr=converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
        std::array<BYTE,4*4*4> pixels{};
        if (SUCCEEDED(hr)) hr=converter->CopyPixels(nullptr,4*4,static_cast<UINT>(pixels.size()),pixels.data());
        bool padded=SUCCEEDED(hr),content=SUCCEEDED(hr);
        for (std::size_t y=0;y<4;++y) for(std::size_t x=0;x<4;++x) {
            const auto pixel=(y*4+x)*4;
            if (x==3 || y==3) padded &= pixels[pixel]==0 && pixels[pixel+1]==0 && pixels[pixel+2]==0 && pixels[pixel+3]==255;
            else content &= pixels[pixel]==0 && pixels[pixel+1]==0 && pixels[pixel+2]==255;
        }
        Check(padded && content,"right/bottom padding is opaque black without stretching the content");
    }
}

}
int main() {
    if (FAILED(::CoInitializeEx(nullptr,COINIT_MULTITHREADED))) return 1;
    const auto directory=std::filesystem::temp_directory_path()/
        (L"srl_gif_中文 空格_"+std::to_wstring(::GetCurrentProcessId())+L"_"+std::to_wstring(::GetTickCount64()));
    std::filesystem::create_directory(directory);
    const auto output=directory/L"动画.gif";
    std::wstring error;
    const srl::OutputSize size{{5,3},{5,3},0,0,true};
    srl::GifWriter writer;
    const bool opened=Open(writer,output,{10,8},RECT{2,2,8,6},size,&error);
    Check(opened,"Unicode output opens exclusively");
    if (!opened) std::wprintf(L"Diagnostic: %ls\n",error.c_str());
    if (opened) {
        auto red=Solid(10,8,{0,0,255});
        // The cropped-away edge must not contaminate the scaled output.
        for (int y=0;y<8;++y) for(int x=0;x<10;++x) {
            if (x>=2 && x<8 && y>=2 && y<6) continue;
            const auto p=static_cast<std::size_t>(y*10+x)*4;
            red[p]=255; red[p+1]=255; red[p+2]=255;
        }
        Check(writer.WriteFrame(red,10,8,0,&error),"first cropped/scaled frame is accepted");
        Check(writer.WriteFrame(Solid(10,8,{0,255,0}),10,8,400'000,&error),"second frame streams the first");
        Check(writer.WriteFrame(Solid(10,8,{255,0,0}),10,8,1'100'000,&error),"third frame streams the second");
        Check(writer.Finish(2'000'000,&error),"finish flushes the pending last-frame duration");
        CheckAnimation(output);
        Check(!writer.WriteFrame(Solid(10,8,{0,0,0}),10,8,3'000'000,&error),"finished writers reject new frames");
    }
    ComPtr<IStream> memory;
    ::CreateStreamOnHGlobal(nullptr,TRUE,memory.GetAddressOf());
    Check(!writer.Open(memory.Get(),{10,8},RECT{-1,0,10,8},size,&error),"invalid crop is rejected");
    Check(!writer.Open(memory.Get(),{std::numeric_limits<int>::max(),8},RECT{0,0,10,8},size,&error),
          "overflowing source stride is rejected");
    Check(!writer.Open(nullptr,{10,8},RECT{0,0,10,8},size,&error),"missing output stream is rejected");
    const auto failed=directory/L"failed.gif";
    if (Open(writer,failed,{10,8},RECT{0,0,10,8},size,&error)) {
        Check(!writer.WriteFrame({},10,8,0,&error),"short frame buffers are rejected");
        Check(::DeleteFileW(failed.c_str())!=FALSE,"failed writer releases its exclusive file handle");
    }
    const auto backwards=directory/L"backwards.gif";
    if (Open(writer,backwards,{10,8},RECT{0,0,10,8},size,&error)) {
        Check(writer.WriteFrame(Solid(10,8,{0,0,0}),10,8,1'000'000,&error),"first delayed capture is accepted");
        Check(!writer.WriteFrame(Solid(10,8,{0,0,0}),10,8,900'000,&error),"backwards capture timestamps are rejected");
        Check(::DeleteFileW(backwards.c_str())!=FALSE,"timestamp failure releases its exclusive file handle");
    }
    const auto empty=directory/L"empty.gif";
    if (Open(writer,empty,{10,8},RECT{0,0,10,8},size,&error)) {
        Check(!writer.Finish(1'000'000,&error),"empty animation is never finalized as success");
        Check(::DeleteFileW(empty.c_str())!=FALSE,"empty animation failure releases its exclusive file handle");
    }
    const auto padded=directory/L"padded.gif";
    if (Open(writer,padded,{3,3},RECT{0,0,3,3},{{3,3},{4,4},1,1,false},&error)) {
        Check(writer.WriteFrame(Solid(3,3,{0,0,255}),3,3,0,&error),"padded animation accepts first frame");
        Check(writer.WriteFrame(Solid(3,3,{255,255,255}),3,3,10'000,&error),"sub20ms transient is coalesced");
        Check(writer.WriteFrame(Solid(3,3,{0,255,0}),3,3,400'000,&error),"normal-duration frame advances animation");
        Check(writer.Finish(6'554'000'000LL,&error),"long final hold splits without overflowing the GIF field");
        CheckTimingAndPadding(padded);
    }
    ComPtr<IStream> occupied;
    ::CreateStreamOnHGlobal(nullptr,TRUE,occupied.GetAddressOf());
    ULONG written=0; occupied->Write("owned",5,&written);
    LARGE_INTEGER start{}; occupied->Seek(start,STREAM_SEEK_SET,nullptr);
    Check(!writer.Open(occupied.Get(),{10,8},RECT{0,0,10,8},size,&error),"nonempty output streams cannot be overwritten");
    std::array<char,5> retained{}; ULONG read=0; occupied->Read(retained.data(),static_cast<ULONG>(retained.size()),&read);
    Check(std::string(retained.data(),read)=="owned","rejected nonempty stream retains its original content");
    writer.Close();
    std::filesystem::remove_all(directory);
    ::CoUninitialize();
    std::printf("%d checks, %d failure(s)\n",checks,failures);
    return failures?1:0;
}
