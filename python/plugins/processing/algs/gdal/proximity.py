# -*- coding: utf-8 -*-

"""
***************************************************************************
    proximity.py
    ---------------------
    Date                 : August 2012
    Copyright            : (C) 2012 by Victor Olaya
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
from builtins import str

__author__ = 'Victor Olaya'
__date__ = 'August 2012'
__copyright__ = '(C) 2012, Victor Olaya'

# This will get replaced with a git SHA1 when you do a git archive

__revision__ = '$Format:%H$'

import os

from qgis.PyQt.QtGui import QIcon

from processing.algs.gdal.GdalAlgorithm import GdalAlgorithm
from processing.core.parameters import ParameterRaster
from processing.core.parameters import ParameterString
from processing.core.parameters import ParameterSelection
from processing.core.parameters import ParameterNumber
from processing.core.outputs import OutputRaster
from processing.tools.system import isWindows
from processing.algs.gdal.GdalUtils import GdalUtils

pluginPath = os.path.split(os.path.split(os.path.dirname(__file__))[0])[0]


class proximity(GdalAlgorithm):

    INPUT = 'INPUT'
    VALUES = 'VALUES'
    UNITS = 'UNITS'
    MAX_DIST = 'MAX_DIST'
    NODATA = 'NODATA'
    BUF_VAL = 'BUF_VAL'
    OUTPUT = 'OUTPUT'
    RTYPE = 'RTYPE'

    TYPE = ['Byte', 'Int16', 'UInt16', 'UInt32', 'Int32', 'Float32', 'Float64']

    DISTUNITS = ['GEO', 'PIXEL']

    def getIcon(self):
        return QIcon(os.path.join(pluginPath, 'images', 'gdaltools', 'proximity.png'))

    def commandLineName(self):
        return "gdal:proximity"

    def defineCharacteristics(self):
        self.name, self.i18n_name = self.trAlgorithm('Proximity (raster distance)')
        self.group, self.i18n_group = self.trAlgorithm('[GDAL] Analysis')
        self.addParameter(ParameterRaster(self.INPUT,
                                          self.tr('Input layer'), False))
        self.addParameter(ParameterString(self.VALUES,
                                          self.tr('Values'), ''))
        self.addParameter(ParameterSelection(self.UNITS,
                                             self.tr('Distance units'), self.DISTUNITS, 0))
        self.addParameter(ParameterNumber(self.MAX_DIST,
                                          self.tr('Max distance (negative value to ignore)'), -1, 9999, -1))
        self.addParameter(ParameterNumber(self.NODATA,
                                          self.tr('Nodata (negative value to ignore)'), -1, 9999, -1))
        self.addParameter(ParameterNumber(self.BUF_VAL,
                                          self.tr('Fixed buf value (negative value to ignore)'),
                                          -1, 9999, -1))
        self.addParameter(ParameterSelection(self.RTYPE,
                                             self.tr('Output raster type'), self.TYPE, 5))
        self.addOutput(OutputRaster(self.OUTPUT, self.tr('Distance')))

    def getConsoleCommands(self):
        output = self.getOutputValue(self.OUTPUT)

        arguments = []
        arguments.append('-ot')
        arguments.append(self.TYPE[self.getParameterValue(self.RTYPE)])
        arguments.append(self.getParameterValue(self.INPUT))
        arguments.append(output)

        arguments.append('-of')
        arguments.append(GdalUtils.getFormatShortNameFromFilename(output))

        arguments.append('-distunits')
        arguments.append(self.DISTUNITS[self.getParameterValue(self.UNITS)])

        values = self.getParameterValue(self.VALUES)
        if len(values) > 0:
            arguments.append('-values')
            arguments.append(values)

        dist = self.getParameterValue(self.MAX_DIST)
        if dist > 0:
            arguments.append('-maxdist')
            arguments.append(str(dist))

        nodata = self.getParameterValue(self.NODATA)
        if nodata > 0:
            arguments.append('-nodata')
            arguments.append(str(nodata))

        buf = self.getParameterValue(self.BUF_VAL)
        if buf > 0:
            arguments.append('-fixed-buf-val')
            arguments.append(str(buf))

        commands = []
        if isWindows():
            commands = ['cmd.exe', '/C ', 'gdal_proximity.bat',
                        GdalUtils.escapeAndJoin(arguments)]
        else:
            commands = ['gdal_proximity.py',
                        GdalUtils.escapeAndJoin(arguments)]

        return commands
