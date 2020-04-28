#include <future>
#include <stdio.h>
#include <limits>
#include <math.h>
#include <memory>
#include <string.h>

#include "getopt/getopt.h"

#include "Bitmap.hpp"
#include "BlockData.hpp"
#include "DataProvider.hpp"
#include "Debug.hpp"
#include "Dither.hpp"
#include "Error.hpp"
#include "System.hpp"
#include "TaskDispatch.hpp"
#include "Timing.hpp"

struct DebugCallback_t : public DebugLog::Callback
{
    void OnDebugMessage( const char* msg ) override
    {
        fprintf( stderr, "%s\n", msg );
    }
} DebugCallback;

void Usage()
{
    fprintf( stderr, "Usage: etcpak [options] input.png {output.pvr}\n" );
    fprintf( stderr, "  Options:\n" );
    fprintf( stderr, "  -v              view mode (loads pvr/ktx file, decodes it and saves to png)\n" );
    fprintf( stderr, "  -s              display image quality measurements\n" );
    fprintf( stderr, "  -b              benchmark mode\n" );
    fprintf( stderr, "  -m              generate mipmaps\n" );
    fprintf( stderr, "  -d              enable dithering\n" );
    fprintf( stderr, "  -a alpha.pvr    save alpha channel in a separate file\n" );
    fprintf( stderr, "  --etc2          enable ETC2 mode\n" );
    fprintf( stderr, "  --rgba          enable ETC2 RGBA mode\n" );
    fprintf( stderr, "  --dxt1          use DXT1 compression\n\n" );
    fprintf( stderr, "Output file name may be unneeded for some modes.\n" );
}

int main( int argc, char** argv )
{
    DebugLog::AddCallback( &DebugCallback );

    bool viewMode = false;
    bool stats = false;
    bool benchmark = false;
    bool mipmap = false;
    bool dither = false;
    bool etc2 = false;
    bool rgba = false;
    bool dxt1 = false;
    const char* alpha = nullptr;
    unsigned int cpus = System::CPUCores();

    if( argc < 3 )
    {
        Usage();
        return 1;
    }

    enum Options
    {
        OptEtc2,
        OptRgba,
        OptDxt1
    };

    struct option longopts[] = {
        { "etc2", no_argument, nullptr, OptEtc2 },
        { "rgba", no_argument, nullptr, OptRgba },
        { "dxt1", no_argument, nullptr, OptDxt1 },
        {}
    };

    int c;
    while( ( c = getopt_long( argc, argv, "vo:a:sbmd", longopts, nullptr ) ) != -1 )
    {
        switch( c )
        {
        case '?':
            Usage();
            return 1;
        case 'v':
            viewMode = true;
            break;
        case 'a':
            alpha = optarg;
            break;
        case 's':
            stats = true;
            break;
        case 'b':
            benchmark = true;
            break;
        case 'm':
            mipmap = true;
            break;
        case 'd':
            dither = true;
            break;
        case OptEtc2:
            etc2 = true;
            break;
        case OptRgba:
            rgba = true;
            etc2 = true;
            break;
        case OptDxt1:
            dxt1 = true;
            break;
        default:
            break;
        }
    }

    if( etc2 && dither )
    {
        printf( "Dithering is disabled in ETC2 mode, as it degrades image quality.\n" );
        dither = false;
    }

    const char* input = nullptr;
    const char* output = nullptr;
    if( benchmark )
    {
        if( argc - optind < 1 )
        {
            Usage();
            return 1;
        }

        input = argv[optind];
    }
    else
    {
        if( argc - optind < 2 )
        {
            Usage();
            return 1;
        }

        input = argv[optind];
        output = argv[optind+1];
    }

    if( benchmark )
    {
        if( viewMode )
        {
            auto bd = std::make_shared<BlockData>( input );

            constexpr int NumTasks = 9;
            uint64_t timeData[NumTasks];
            for( int i=0; i<NumTasks; i++ )
            {
                const auto start = GetTime();
                auto res = bd->Decode();
                const auto end = GetTime();
                timeData[i] = end - start;
            }
            std::sort( timeData, timeData+NumTasks );
            const auto median = timeData[NumTasks/2] / 1000.f;
            printf( "Median decode time for %i runs: %0.3f ms (%0.3f Mpx/s)\n", NumTasks, median, bd->Size().x * bd->Size().y / ( median * 1000 ) );
        }
        else
        {
            auto start = GetTime();
            auto bmp = std::make_shared<Bitmap>( input, std::numeric_limits<unsigned int>::max(), !dxt1 );
            auto data = bmp->Data();
            auto end = GetTime();
            printf( "Image load time: %0.3f ms\n", ( end - start ) / 1000.f );

            constexpr int NumTasks = 9;
            uint64_t timeData[NumTasks];
            for( int i=0; i<NumTasks; i++ )
            {
                BlockData::Type type;
                Channels channel;
                if( alpha ) channel = Channels::Alpha;
                else channel = Channels::RGB;
                if( rgba ) type = BlockData::Etc2_RGBA;
                else if( etc2 ) type = BlockData::Etc2_RGB;
                else if( dxt1 ) type = BlockData::Dxt1;
                else type = BlockData::Etc1;
                auto bd = std::make_shared<BlockData>( bmp->Size(), false, type );
                const auto localStart = GetTime();
                if( rgba )
                {
                    bd->ProcessRGBA( bmp->Data(), bmp->Size().x * bmp->Size().y / 16, 0, bmp->Size().x );
                }
                else
                {
                    bd->Process( bmp->Data(), bmp->Size().x * bmp->Size().y / 16, 0, bmp->Size().x, channel, dither );
                }
                const auto localEnd = GetTime();
                timeData[i] = localEnd - localStart;
            }
            std::sort( timeData, timeData+NumTasks );
            const auto median = timeData[NumTasks/2] / 1000.f;
            printf( "Median compression time for %i runs: %0.3f ms (%0.3f Mpx/s)\n", NumTasks, median, bmp->Size().x * bmp->Size().y / ( median * 1000 ) );
        }
    }
    else if( viewMode )
    {
        auto bd = std::make_shared<BlockData>( input );
        auto out = bd->Decode();
        out->Write( output );
    }
    else
    {
        DataProvider dp( input, mipmap, !dxt1 );
        auto num = dp.NumberOfParts();

        BlockData::Type type;
        if( etc2 )
        {
            if( rgba && dp.Alpha() )
            {
                type = BlockData::Etc2_RGBA;
            }
            else
            {
                type = BlockData::Etc2_RGB;
            }
        }
        else if( dxt1 )
        {
            type = BlockData::Dxt1;
        }
        else
        {
            type = BlockData::Etc1;
        }

        TaskDispatch taskDispatch( cpus );

        auto bd = std::make_shared<BlockData>( output, dp.Size(), mipmap, type );
        BlockDataPtr bda;
        if( alpha && dp.Alpha() && !rgba )
        {
            bda = std::make_shared<BlockData>( alpha, dp.Size(), mipmap, type );
        }
        for( int i=0; i<num; i++ )
        {
            auto part = dp.NextPart();

            if( type == BlockData::Etc2_RGBA )
            {
                TaskDispatch::Queue( [part, i, &bd, &dither]()
                {
                    bd->ProcessRGBA( part.src, part.width / 4 * part.lines, part.offset, part.width );
                } );
            }
            else
            {
                TaskDispatch::Queue( [part, i, &bd, &dither]()
                {
                    bd->Process( part.src, part.width / 4 * part.lines, part.offset, part.width, Channels::RGB, dither );
                } );
                if( bda )
                {
                    TaskDispatch::Queue( [part, i, &bda]()
                    {
                        bda->Process( part.src, part.width / 4 * part.lines, part.offset, part.width, Channels::Alpha, false );
                    } );
                }
            }
        }

        TaskDispatch::Sync();

        if( stats )
        {
            auto out = bd->Decode();
            float mse = CalcMSE3( dp.ImageData(), *out );
            printf( "RGB data\n" );
            printf( "  RMSE: %f\n", sqrt( mse ) );
            printf( "  PSNR: %f\n", 20 * log10( 255 ) - 10 * log10( mse ) );
        }
    }

    return 0;
}
