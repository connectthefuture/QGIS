# -*- coding: utf-8 -*-

"""
***************************************************************************
    raster.py
    ---------------------
    Date                 : February 2013
    Copyright            : (C) 2013 by Victor Olaya  and Alexander Bruy
    Email                : volayaf at gmail dot com
***************************************************************************
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
***************************************************************************
"""

__author__ = 'Victor Olaya  and Alexander Bruy'
__date__ = 'February 2013'
__copyright__ = '(C) 2013, Victor Olaya  and Alexander Bruy'

# This will get replaced with a git SHA1 when you do a git archive

__revision__ = '$Format:%H$'

from builtins import str
from builtins import range
from builtins import object

import os
import struct

import numpy
from osgeo import gdal

from processing.core.GeoAlgorithmExecutionException import GeoAlgorithmExecutionException


RASTER_EXTENSION_MAP = None


def initGdalData():
    global RASTER_EXTENSION_MAP

    if RASTER_EXTENSION_MAP is not None:
        return

    if gdal.GetDriverCount() == 0:
        gdal.AllRegister()

    RASTER_EXTENSION_MAP = dict()
    for i in range(gdal.GetDriverCount()):
        driver = gdal.GetDriver(i)
        if driver is None:
            continue
        md = driver.GetMetadata()
        if gdal.DCAP_CREATE in md and md[gdal.DCAP_CREATE].lower() == 'yes':
            ext = md[gdal.DMD_EXTENSION] if gdal.DMD_EXTENSION in md else None
            if ext is not None and ext != '':
                RASTER_EXTENSION_MAP[driver.ShortName] = ext


def formatShortNameFromFileName(fileName):
    initGdalData()
    ext = os.path.splitext(fileName)[1][1:]
    for k, v in RASTER_EXTENSION_MAP.items():
        if ext == v:
            return k
    return 'GTiff'


def scanraster(layer, progress):
    filename = str(layer.source())
    dataset = gdal.Open(filename, gdal.GA_ReadOnly)
    band = dataset.GetRasterBand(1)
    nodata = band.GetNoDataValue()
    bandtype = gdal.GetDataTypeName(band.DataType)
    for y in range(band.YSize):
        progress.setPercentage(y / float(band.YSize) * 100)
        scanline = band.ReadRaster(0, y, band.XSize, 1, band.XSize, 1,
                                   band.DataType)
        if bandtype == 'Byte':
            values = struct.unpack('B' * band.XSize, scanline)
        elif bandtype == 'Int16':
            values = struct.unpack('h' * band.XSize, scanline)
        elif bandtype == 'UInt16':
            values = struct.unpack('H' * band.XSize, scanline)
        elif bandtype == 'Int32':
            values = struct.unpack('i' * band.XSize, scanline)
        elif bandtype == 'UInt32':
            values = struct.unpack('I' * band.XSize, scanline)
        elif bandtype == 'Float32':
            values = struct.unpack('f' * band.XSize, scanline)
        elif bandtype == 'Float64':
            values = struct.unpack('d' * band.XSize, scanline)
        else:
            raise GeoAlgorithmExecutionException('Raster format not supported')
        for value in values:
            if value == nodata:
                value = None
            yield value


def mapToPixel(mX, mY, geoTransform):
    try:
        # GDAL 1.x
        (pX, pY) = gdal.ApplyGeoTransform(
            gdal.InvGeoTransform(geoTransform)[1], mX, mY)
    except TypeError:
        # GDAL 2.x
        (pX, pY) = gdal.ApplyGeoTransform(
            gdal.InvGeoTransform(geoTransform), mX, mY)
    return (int(pX), int(pY))


def pixelToMap(pX, pY, geoTransform):
    return gdal.ApplyGeoTransform(geoTransform, pX + 0.5, pY + 0.5)


class RasterWriter(object):

    NODATA = -99999.0

    def __init__(self, fileName, minx, miny, maxx, maxy, cellsize,
                 nbands, crs, geotransform=None):
        self.fileName = fileName
        self.nx = int((maxx - minx) / float(cellsize))
        self.ny = int((maxy - miny) / float(cellsize))
        self.nbands = nbands
        self.matrix = numpy.empty(shape=(self.ny, self.nx), dtype=numpy.float32)
        self.matrix.fill(self.NODATA)
        self.cellsize = cellsize
        self.crs = crs
        self.minx = minx
        self.maxy = maxy
        self.geotransform = geotransform

    def setValue(self, value, x, y, band=0):
        try:
            self.matrix[y, x] = value
        except IndexError:
            pass

    def getValue(self, x, y, band=0):
        try:
            return self.matrix[y, x]
        except IndexError:
            return self.NODATA

    def close(self):
        fmt = 'GTiff'
        driver = gdal.GetDriverByName(fmt)
        dst_ds = driver.Create(self.fileName, self.nx, self.ny, 1,
                               gdal.GDT_Float32)
        dst_ds.SetProjection(str(self.crs.toWkt()))
        if self.geotransform is None:
            dst_ds.SetGeoTransform([self.minx, self.cellsize, 0,
                                    self.maxy, self.cellsize, 0])
        else:
            dst_ds.SetGeoTransform(self.geotransform)
        dst_ds.GetRasterBand(1).WriteArray(self.matrix)
        dst_ds = None
