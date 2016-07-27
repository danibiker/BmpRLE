#include <iostream>

using namespace std;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <math.h>
#include <Constant.h>
#include <httputil.h>
#include "ImagenGestor.h"
#include "image/uiimgdownloader.h"

#define BUFFPIXEL 30
SDL_Surface *screen;

// These read 16- and 32-bit types from the SD card file.
// BMP data is stored little-endian, Arduino is little-endian too.
// May need to reverse subscript order if porting elsewhere.
uint16_t read16(FILE *f) {
  uint16_t result;
  uint8_t buffer;
  fread(&buffer, 1, 1, f);
  ((uint8_t *)&result)[0] = buffer; // LSB
  fread(&buffer, 1, 1, f);
  ((uint8_t *)&result)[1] = buffer; // MSB
  return result;
}

uint32_t read32(FILE *f) {
  uint32_t result;
  uint8_t buffer;
  fread(&buffer, 1, 1, f);

  ((uint8_t *)&result)[0] = buffer; // LSB
  fread(&buffer, 1, 1, f);
  ((uint8_t *)&result)[1] = buffer;
  fread(&buffer, 1, 1, f);
  ((uint8_t *)&result)[2] = buffer;
  fread(&buffer, 1, 1, f);
  ((uint8_t *)&result)[3] = buffer; // MSB
  return result;
}

uint32_t read32_(FILE *f) {
  uint32_t result;
  if (fread(&result, 4, 1, f) != 0)
    return result;
  else
    return 0;
}

uint8_t read8(FILE *f) {
  uint8_t buffer;
  fread(&buffer, 1, 1, f);
  return buffer;
}

uint32_t read24(FILE *f) {
  uint32_t result;
  uint8_t buffer;
  fread(&buffer, 1, 1, f);
  ((uint8_t *)&result)[0] = buffer; // LSB
  fread(&buffer, 1, 1, f);
  ((uint8_t *)&result)[1] = buffer;
  fread(&buffer, 1, 1, f);
  ((uint8_t *)&result)[2] = buffer;
  ((uint8_t *)&result)[3] = 0x00; // MSB
  return result;
}


// Pass 8-bit (each) R,G,B, get back 16-bit packed color
uint16_t Color565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/*
 * Set the pixel at (x, y) to the given value
 * NOTE: The surface must be locked before calling this!
 */
void putpixel(SDL_Surface *surface, int x, int y, Uint32 pixel)
{
    int bpp = surface->format->BytesPerPixel;
    /* Here p is the address to the pixel we want to set */
    Uint8 *p = (Uint8 *)surface->pixels + y * surface->pitch + x * bpp;

    switch(bpp) {
    case 1:
        *p = pixel;
        break;

    case 2:
        *(Uint16 *)p = pixel;
        break;

    case 3:
        if(SDL_BYTEORDER == SDL_BIG_ENDIAN) {
            p[0] = (pixel >> 16) & 0xff;
            p[1] = (pixel >> 8) & 0xff;
            p[2] = pixel & 0xff;
        } else {
            p[0] = pixel & 0xff;
            p[1] = (pixel >> 8) & 0xff;
            p[2] = (pixel >> 16) & 0xff;
        }
        break;

    case 4:
        *(Uint32 *)p = pixel;
        break;
    }
}

/**
*
*/
unsigned long convertTo565(string filename){
    int      bmpWidth, bmpHeight;   // W+H in pixels
    uint8_t  bmpDepth;              // Bit depth (currently must be 24)
    uint32_t bmpImageoffset;        // Start of image data in file
    uint32_t rowSize;               // Not always = bmpWidth; may have padding
    uint8_t  sdbuffer[3*BUFFPIXEL]; // pixel buffer (R+G+B per pixel)
    uint8_t  buffidx = sizeof(sdbuffer); // Current position in sdbuffer
    bool  flip    = true;        // BMP is stored bottom-to-top
    int      w, h, row, col;
    uint8_t  r, g, b;
    long pos = 0;
    int bmpSize = 0;

    FILE *bmpFile, *outBmpFile;
    bmpFile = fopen(filename.c_str(),"rb");

    unsigned int fpos = filename.find_last_of(".");
    string out;
    if (fpos != string::npos){
        out = filename.substr(0, fpos) + ".565";
    } else {
        out = filename + ".565";
    }
    outBmpFile = fopen(out.c_str(),"wb+");

    uint16_t color;
    size_t totalLength = 0;

    if(read16(bmpFile) == 0x4D42) { // BMP signature
        bmpSize = read32(bmpFile);
        cout << "bmpSize: " << bmpSize << endl;
        (void)read32(bmpFile); // Read & ignore creator bytes
        bmpImageoffset = read32(bmpFile); // Start of image data
        cout << "Image Offset: " << bmpImageoffset << endl;
        // Read DIB header
        cout << "Header size: " << read32(bmpFile) << endl;
        bmpWidth  = read32(bmpFile);
        bmpHeight = read32(bmpFile);
        cout << bmpWidth << "x" << bmpHeight << endl;
        int planes = read16(bmpFile);
        cout << "planes: " << planes << endl;
        if(planes == 1) { // # planes -- must be '1'
          bmpDepth = read16(bmpFile); // bits per pixel
          cout << "Bit Depth: " << bmpDepth << endl;
          if((bmpDepth == 24) && (read32(bmpFile) == 0)) { // 0 = uncompressed
            rowSize = (bmpWidth * 3 + 3) & ~3;
            // If bmpHeight is negative, image is in top-down order.
            // This is not canon but has been observed in the wild.
            if(bmpHeight < 0) {
              bmpHeight = -bmpHeight;
              flip      = false;
            }
            // Crop area to be loaded
            w = bmpWidth;
            h = bmpHeight;

            for (row=0; row<h; row++) { // For each scanline...

              // Seek to start of scan line.  It might seem labor-
              // intensive to be doing this on every line, but this
              // method covers a lot of gritty details like cropping
              // and scanline padding.  Also, the seek only takes
              // place if the file position actually needs to change
              // (avoids a lot of cluster math in SD library).
              if(flip) // Bitmap is stored bottom-to-top order (normal BMP)
                pos = bmpImageoffset + (bmpHeight - 1 - row) * rowSize;
              else     // Bitmap is stored top-to-bottom
                pos = bmpImageoffset + row * rowSize;


              if(ftell(bmpFile) != pos) { // Need seek?
                fseek(bmpFile, pos, SEEK_SET);
                buffidx = sizeof(sdbuffer); // Force buffer reload
              }

              for (col=0; col<w; col++) { // For each pixel...
                // Time to read more pixel data?
                if (buffidx >= sizeof(sdbuffer)) { // Indeed
                  fread(sdbuffer, 1, sizeof(sdbuffer), bmpFile);
                  buffidx = 0; // Set index to beginning
                }

                // Convert pixel from BMP to TFT format, push to display
                b = sdbuffer[buffidx++];
                g = sdbuffer[buffidx++];
                r = sdbuffer[buffidx++];
                //color = SDL_MapRGB(screen->format, r,g,b);
                color = Color565(r,g,b);
                fwrite(&color, sizeof(color), 1, outBmpFile);
              } // end pixel
            } // end scanline
          }
        }
    }

    fclose(bmpFile);
    fclose(outBmpFile);
    cout << endl;
    unsigned long totalKB = (totalLength * sizeof(color))/1024;
    cout << "totalLength: " << totalKB << "Kbytes" << endl;
    return totalKB;
}

/**
*
*/
int toScreen565(string filename, int x, int y, int lcdw, int lcdh){
    uint16_t pixelValue = 0;
    int scrX = 0;
    int scrY = 0;

    FILE *bmpFile = fopen(filename.c_str(),"rb");
    if (bmpFile == NULL){
        return 0;
    }

    for (scrY=y; scrY < lcdh; scrY++){
        for (scrX=x; scrX < lcdw; scrX++){
            pixelValue = read16(bmpFile);
            putpixel(screen, scrX, scrY, pixelValue);
        }
    }
    fclose(bmpFile);
    return 1;
}

/**
*
*/
unsigned long convertTo565Rle(string filename){
    int      bmpWidth, bmpHeight;   // W+H in pixels
    uint8_t  bmpDepth;              // Bit depth (currently must be 24)
    uint32_t bmpImageoffset;        // Start of image data in file
    uint32_t rowSize;               // Not always = bmpWidth; may have padding
    uint8_t  sdbuffer[3*BUFFPIXEL]; // pixel buffer (R+G+B per pixel)
    uint8_t  buffidx = sizeof(sdbuffer); // Current position in sdbuffer
    bool  flip    = true;        // BMP is stored bottom-to-top
    int      w, h, row, col;
    uint8_t  r, g, b;
    long pos = 0;
    int bmpSize = 0;

    FILE *bmpFile, *outBmpFile;
    bmpFile = fopen(filename.c_str(),"rb");

    unsigned int fpos = filename.find_last_of(".");
    string out;
    if (fpos != string::npos){
        out = filename.substr(0, fpos) + ".r65";
    } else {
        out = filename + ".r65";
    }
    outBmpFile = fopen(out.c_str(),"wb+");

    struct t_rle{
        uint16_t color;
        uint8_t  r, g, b;
        uint32_t n;
    };

    uint16_t color;
    t_rle lastColor;
    lastColor.n = 0;
    size_t totalLength = 0;
    int counter = 0;

    uint32_t maxRep = pow(2, sizeof(lastColor.n) * 8) - 1;
    cout << "maxRep: " << maxRep << "," << sizeof(lastColor.n) << "," << sizeof(lastColor.color) << endl;

    if(read16(bmpFile) == 0x4D42) { // BMP signature
        bmpSize = read32(bmpFile);
        cout << "bmpSize: " << bmpSize << endl;
        (void)read32(bmpFile); // Read & ignore creator bytes
        bmpImageoffset = read32(bmpFile); // Start of image data
        cout << "Image Offset: " << bmpImageoffset << endl;
        // Read DIB header
        cout << "Header size: " << read32(bmpFile) << endl;
        bmpWidth  = read32(bmpFile);
        bmpHeight = read32(bmpFile);
        cout << bmpWidth << "x" << bmpHeight << endl;
        int planes = read16(bmpFile);
        cout << "planes: " << planes << endl;
        if(planes == 1) { // # planes -- must be '1'
          bmpDepth = read16(bmpFile); // bits per pixel
          cout << "Bit Depth: " << bmpDepth << endl;
          if((bmpDepth == 24) && (read32(bmpFile) == 0)) { // 0 = uncompressed

            //int fPos = ftell(bmpFile);
            //fseek(bmpFile, 0, SEEK_SET);
            //unsigned char ftmp[fPos];
            //Guardamos en el destino toda la cabecera leida hasta ahora
            //fread(ftmp, 1, fPos, bmpFile);

            /**Se escribe la cabecera*/
            //fwrite(ftmp, 1, fPos, outBmpFile);
            //fseek(bmpFile, fPos, SEEK_SET);
            // BMP rows are padded (if needed) to 4-byte boundary
            rowSize = (bmpWidth * 3 + 3) & ~3;

            // If bmpHeight is negative, image is in top-down order.
            // This is not canon but has been observed in the wild.
            if(bmpHeight < 0) {
              bmpHeight = -bmpHeight;
              flip      = false;
            }

            // Crop area to be loaded
            w = bmpWidth;
            h = bmpHeight;


            for (row=0; row<h; row++) { // For each scanline...

              // Seek to start of scan line.  It might seem labor-
              // intensive to be doing this on every line, but this
              // method covers a lot of gritty details like cropping
              // and scanline padding.  Also, the seek only takes
              // place if the file position actually needs to change
              // (avoids a lot of cluster math in SD library).
              if(flip) // Bitmap is stored bottom-to-top order (normal BMP)
                pos = bmpImageoffset + (bmpHeight - 1 - row) * rowSize;
              else     // Bitmap is stored top-to-bottom
                pos = bmpImageoffset + row * rowSize;


              if(ftell(bmpFile) != pos) { // Need seek?
                fseek(bmpFile, pos, SEEK_SET);
                buffidx = sizeof(sdbuffer); // Force buffer reload
              }

              for (col=0; col<w; col++) { // For each pixel...
                // Time to read more pixel data?
                if (buffidx >= sizeof(sdbuffer)) { // Indeed
                  fread(sdbuffer, 1, sizeof(sdbuffer), bmpFile);
                  buffidx = 0; // Set index to beginning
                }
                // Convert pixel from BMP to TFT format, push to display
                b = sdbuffer[buffidx++];
                g = sdbuffer[buffidx++];
                r = sdbuffer[buffidx++];
                //tft.pushColor(tft.Color565(r,g,b));
                //color = SDL_MapRGB(screen->format, r,g,b);
                color = Color565(r,g,b);
                //putpixel(screen, col, row, color);
                //fwrite(&pix, 4, 1, outBmpFile);
                if (row == 0 && col == 0){
                    lastColor.n = 1;
                    lastColor.color = color;
                } else if (lastColor.color != color || lastColor.n >= maxRep){
                    fwrite(&lastColor.n, sizeof(lastColor.n), 1, outBmpFile);
                    fwrite(&lastColor.color, sizeof(lastColor.color), 1, outBmpFile);
                    counter += lastColor.n;
                    lastColor.n = 1;
                    lastColor.color = color;
                    totalLength++;
                } else {
                    lastColor.n++;
                }
              } // end pixel
            } // end scanline

            if ((lastColor.color == color || lastColor.n >= maxRep)){
                fwrite(&lastColor.n, sizeof(lastColor.n),1, outBmpFile);
                fwrite(&lastColor.color, sizeof(lastColor.color),1, outBmpFile);
                lastColor.n = 1;
                lastColor.color = color;
                totalLength++;
            }
          }
        }
    }

    fclose(bmpFile);
    fclose(outBmpFile);
    cout << endl;

    unsigned long totalKB = (totalLength * sizeof(lastColor.n) + totalLength * sizeof(lastColor.color))/1024;
    cout << "totalLength: " << totalKB << "Kbytes" << endl;
    cout << "pixels leidos: " << counter << " counter: " << counter <<  endl;
    return totalKB;
}


/**
*
*/
int rleFileToScreen(string filename, int x, int y, int lcdw, int lcdh){
    uint32_t repetitions = 0;
    uint16_t pixelValue = 0;

    int counter = 0;
    int scrX = 0;
    int scrY = 0;

    FILE *bmpFile = fopen(filename.c_str(),"rb");
    if (bmpFile == NULL){
        return 0;
    }

    int posFile = 0;
    while (counter < lcdw * lcdh){
        repetitions = read32(bmpFile);
        pixelValue = read16(bmpFile);
        //cout << "repetitions: " << repetitions << endl;
        if (repetitions == 0){
            cout << "problema en counter: " << counter << endl;
            return 0;
        }

        while(repetitions > 0){
            scrX = counter % (lcdw);
            scrY = counter / (lcdw);
            //cout << "scrX: " << scrX << " scrY: " << scrY << endl;
            putpixel(screen, scrX, scrY, pixelValue);
            repetitions--;
            counter++;
        }
        posFile++;
    }
    fclose(bmpFile);
    return 1;
}

unsigned long surfaceTo565(SDL_Surface *mySurface){
    int bytesPixel = mySurface->format->BytesPerPixel;

    Traza::print("Bytes por pixel",bytesPixel, W_DEBUG);
    return 0;

}

/**
*
*/
void downloadMap(string url){
    Constant::setPROXYIP("10.129.8.100");
    Constant::setPROXYPORT("8080");
    Constant::setPROXYUSER("dmarcobo");
    Constant::setPROXYPASS("bC6E4X0V3c");

    HttpUtil utilHttp;
    bool ret = utilHttp.download(url);

    if (!ret){
        Traza::print("Imposible conectar a: " + url, W_ERROR);
    } else {
        //Redimensionamos la imagen al tamanyo indicado
        SDL_Surface *mySurface = NULL;
        ImagenGestor imgGestor;
        Traza::print("Cargando imagen de bytes: ",utilHttp.getDataLength() , W_DEBUG);
        imgGestor.loadImgFromMem(utilHttp.getRawData(), utilHttp.getDataLength(), &mySurface);

        if (mySurface != NULL){
              Traza::print("Imagen obtenida correctamente: " + url, W_DEBUG);
              surfaceTo565(mySurface);
        } else {
            Traza::print("Error al obtener la imagen: " + url, W_ERROR);
        }
    }
    cout << utilHttp.getDataLength() << endl;
}



/**
*
*/
int main(int argc, char *argv[]){
    #ifdef WIN
        string appDir = argv[0];
        int pos = appDir.rfind(Constant::getFileSep());
        if (pos == string::npos){
            FILE_SEPARATOR = FILE_SEPARATOR_UNIX;
            pos = appDir.rfind(FILE_SEPARATOR);
            tempFileSep[0] = FILE_SEPARATOR;
        }
        appDir = appDir.substr(0, pos);
        if (appDir[appDir.length()-1] == '.'){
            appDir.substr(0, appDir.rfind(Constant::getFileSep()));
        }
        Constant::setAppDir(appDir);
    #endif // WIN

    #ifdef UNIX
        Dirutil dir;
        Constant::setAppDir(dir.getDirActual());
    #endif // UNIX

    string rutaTraza = appDir + Constant::getFileSep() + "Traza.txt";

    Traza *traza = new Traza(rutaTraza.c_str());
    /* Initialize the SDL library */
    if( SDL_Init(SDL_INIT_VIDEO) < 0 ) {
        fprintf(stderr,
                "Couldn't initialize SDL: %s\n", SDL_GetError());
        exit(1);
    }

    /* Clean up on exit */
    atexit(SDL_Quit);
    /*
     * Initialize the display in a 640x480 8-bit palettized mode,
     * requesting a software surface
     */
    screen = SDL_SetVideoMode(256, 256, 16, SDL_SWSURFACE);
    if ( screen == NULL ) {
        fprintf(stderr, "Couldn't set 640x480x8 video mode: %s\n",
                        SDL_GetError());
        exit(1);
    }

    //convertTo565("C:\\Users\\dmarcobo\\Desktop\\Maperitive\\Tiles\\mapnikTiles\\16\\Imagen1.bmp");
    //convertTo565Rle("C:\\Users\\dmarcobo\\Desktop\\Maperitive\\Tiles\\mapnikTiles\\16\\Imagen1.bmp");
    //rleFileToScreen("C:\\Users\\dmarcobo\\Desktop\\Maperitive\\Tiles\\mapnikTiles\\16\\Imagen1.r65", 0, 0, 256, 256);
    //toScreen565("C:\\Users\\dmarcobo\\Desktop\\Maperitive\\Tiles\\mapnikTiles\\16\\Imagen1.565", 0, 0, 256, 256);

    downloadMap("http://b.tile.opencyclemap.org/cycle/16/32692/25161.png");

    bool salir = false;
    while(!salir){
        SDL_Event event;
        if(SDL_PollEvent(&event)) {
            if (event.type == SDL_KEYDOWN){
                salir = true;
            }
        }

        //rleToScreen("C:\\Users\\dmarcobo\\Desktop\\Maperitive\\Tiles\\mapnikTiles\\16\\25160.bmp_rle.bmp", 0, 0, 128, 160);
        //showMap();
        SDL_UpdateRect(screen, 0, 0, 0, 0);
    }


    return 0;
}
