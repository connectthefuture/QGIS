/***************************************************************************
                          qgsvectorlayerjoinbuffer.cpp
                          ----------------------------
    begin                : Feb 09, 2011
    copyright            : (C) 2011 by Marco Hugentobler
    email                : marco dot hugentobler at sourcepole dot ch
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsvectorlayerjoinbuffer.h"

#include "qgsfeatureiterator.h"
#include "qgslogger.h"
#include "qgsproject.h"
#include "qgsvectordataprovider.h"

#include <QDomElement>

QgsVectorLayerJoinBuffer::QgsVectorLayerJoinBuffer( QgsVectorLayer* layer )
    : mLayer( layer )
{
}

static QList<QgsVectorLayer*> _outEdges( QgsVectorLayer* vl )
{
  QList<QgsVectorLayer*> lst;
  Q_FOREACH ( const QgsVectorJoinInfo& info, vl->vectorJoins() )
  {
    if ( QgsVectorLayer* joinVl = qobject_cast<QgsVectorLayer*>( QgsProject::instance()->mapLayer( info.joinLayerId ) ) )
      lst << joinVl;
  }
  return lst;
}

static bool _hasCycleDFS( QgsVectorLayer* n, QHash<QgsVectorLayer*, int>& mark )
{
  if ( mark.value( n ) == 1 ) // temporary
    return true;
  if ( mark.value( n ) == 0 ) // not visited
  {
    mark[n] = 1; // temporary
    Q_FOREACH ( QgsVectorLayer* m, _outEdges( n ) )
    {
      if ( _hasCycleDFS( m, mark ) )
        return true;
    }
    mark[n] = 2; // permanent
  }
  return false;
}


bool QgsVectorLayerJoinBuffer::addJoin( const QgsVectorJoinInfo& joinInfo )
{
  QMutexLocker locker( &mMutex );
  mVectorJoins.push_back( joinInfo );

  // run depth-first search to detect cycles in the graph of joins between layers.
  // any cycle would cause infinite recursion when updating fields
  QHash<QgsVectorLayer*, int> markDFS;
  if ( mLayer && _hasCycleDFS( mLayer, markDFS ) )
  {
    // we have to reject this one
    mVectorJoins.pop_back();
    return false;
  }

  //cache joined layer to virtual memory if specified by user
  if ( joinInfo.memoryCache )
  {
    cacheJoinLayer( mVectorJoins.last() );
  }

  // Wait for notifications about changed fields in joined layer to propagate them.
  // During project load the joined layers possibly do not exist yet so the connection will not be created,
  // but then QgsProject makes sure to call createJoinCaches() which will do the connection.
  // Unique connection makes sure we do not respond to one layer's update more times (in case of multiple join)
  if ( QgsVectorLayer* vl = qobject_cast<QgsVectorLayer*>( QgsProject::instance()->mapLayer( joinInfo.joinLayerId ) ) )
  {
    connect( vl, &QgsVectorLayer::updatedFields, this, &QgsVectorLayerJoinBuffer::joinedLayerUpdatedFields, Qt::UniqueConnection );
    connect( vl, &QgsVectorLayer::layerModified, this, &QgsVectorLayerJoinBuffer::joinedLayerModified, Qt::UniqueConnection );
  }

  emit joinedFieldsChanged();
  return true;
}


bool QgsVectorLayerJoinBuffer::removeJoin( const QString& joinLayerId )
{
  QMutexLocker locker( &mMutex );
  bool res = false;
  for ( int i = 0; i < mVectorJoins.size(); ++i )
  {
    if ( mVectorJoins.at( i ).joinLayerId == joinLayerId )
    {
      mVectorJoins.removeAt( i );
      res = true;
    }
  }

  if ( QgsVectorLayer* vl = qobject_cast<QgsVectorLayer*>( QgsProject::instance()->mapLayer( joinLayerId ) ) )
  {
    disconnect( vl, &QgsVectorLayer::updatedFields, this, &QgsVectorLayerJoinBuffer::joinedLayerUpdatedFields );
  }

  emit joinedFieldsChanged();
  return res;
}

void QgsVectorLayerJoinBuffer::cacheJoinLayer( QgsVectorJoinInfo& joinInfo )
{
  //memory cache not required or already done
  if ( !joinInfo.memoryCache || !joinInfo.cacheDirty )
  {
    return;
  }

  QgsVectorLayer* cacheLayer = dynamic_cast<QgsVectorLayer*>( QgsProject::instance()->mapLayer( joinInfo.joinLayerId ) );
  if ( cacheLayer )
  {
    int joinFieldIndex;
    if ( joinInfo.joinFieldName.isEmpty() )
      joinFieldIndex = joinInfo.joinFieldIndex;   //for compatibility with 1.x
    else
      joinFieldIndex = cacheLayer->fields().indexFromName( joinInfo.joinFieldName );

    if ( joinFieldIndex < 0 || joinFieldIndex >= cacheLayer->fields().count() )
      return;

    joinInfo.cachedAttributes.clear();

    QgsFeatureRequest request;
    request.setFlags( QgsFeatureRequest::NoGeometry );

    // maybe user requested just a subset of layer's attributes
    // so we do not have to cache everything
    bool hasSubset = joinInfo.joinFieldNamesSubset();
    QVector<int> subsetIndices;
    if ( hasSubset )
    {
      subsetIndices = joinSubsetIndices( cacheLayer, *joinInfo.joinFieldNamesSubset() );

      // we need just subset of attributes - but make sure to include join field name
      QgsAttributeList cacheLayerAttrs = subsetIndices.toList();
      if ( !cacheLayerAttrs.contains( joinFieldIndex ) )
        cacheLayerAttrs.append( joinFieldIndex );
      request.setSubsetOfAttributes( cacheLayerAttrs );
    }

    QgsFeatureIterator fit = cacheLayer->getFeatures( request );
    QgsFeature f;
    while ( fit.nextFeature( f ) )
    {
      QgsAttributes attrs = f.attributes();
      QString key = attrs.at( joinFieldIndex ).toString();
      if ( hasSubset )
      {
        QgsAttributes subsetAttrs( subsetIndices.count() );
        for ( int i = 0; i < subsetIndices.count(); ++i )
          subsetAttrs[i] = attrs.at( subsetIndices.at( i ) );
        joinInfo.cachedAttributes.insert( key, subsetAttrs );
      }
      else
      {
        QgsAttributes attrs2 = attrs;
        attrs2.remove( joinFieldIndex );  // skip the join field to avoid double field names (fields often have the same name)
        joinInfo.cachedAttributes.insert( key, attrs2 );
      }
    }
    joinInfo.cacheDirty = false;
  }
}


QVector<int> QgsVectorLayerJoinBuffer::joinSubsetIndices( QgsVectorLayer* joinLayer, const QStringList& joinFieldsSubset )
{
  QVector<int> subsetIndices;
  const QgsFields& fields = joinLayer->fields();
  for ( int i = 0; i < joinFieldsSubset.count(); ++i )
  {
    QString joinedFieldName = joinFieldsSubset.at( i );
    int index = fields.lookupField( joinedFieldName );
    if ( index != -1 )
    {
      subsetIndices.append( index );
    }
    else
    {
      QgsDebugMsg( "Join layer subset field not found: " + joinedFieldName );
    }
  }

  return subsetIndices;
}

void QgsVectorLayerJoinBuffer::updateFields( QgsFields& fields )
{
  QString prefix;

  QList< QgsVectorJoinInfo>::const_iterator joinIt = mVectorJoins.constBegin();
  for ( int joinIdx = 0 ; joinIt != mVectorJoins.constEnd(); ++joinIt, ++joinIdx )
  {
    QgsVectorLayer* joinLayer = qobject_cast<QgsVectorLayer*>( QgsProject::instance()->mapLayer( joinIt->joinLayerId ) );
    if ( !joinLayer )
    {
      continue;
    }

    const QgsFields& joinFields = joinLayer->fields();
    QString joinFieldName;
    if ( joinIt->joinFieldName.isEmpty() && joinIt->joinFieldIndex >= 0 && joinIt->joinFieldIndex < joinFields.count() )
      joinFieldName = joinFields.field( joinIt->joinFieldIndex ).name();  //for compatibility with 1.x
    else
      joinFieldName = joinIt->joinFieldName;

    QSet<QString> subset;
    bool hasSubset = false;
    if ( joinIt->joinFieldNamesSubset() )
    {
      hasSubset = true;
      subset = QSet<QString>::fromList( *joinIt->joinFieldNamesSubset() );
    }

    if ( joinIt->prefix.isNull() )
    {
      prefix = joinLayer->name() + '_';
    }
    else
    {
      prefix = joinIt->prefix;
    }

    for ( int idx = 0; idx < joinFields.count(); ++idx )
    {
      // if using just a subset of fields, filter some of them out
      if ( hasSubset && !subset.contains( joinFields.at( idx ).name() ) )
        continue;

      //skip the join field to avoid double field names (fields often have the same name)
      // when using subset of field, use all the selected fields
      if ( hasSubset || joinFields.at( idx ).name() != joinFieldName )
      {
        QgsField f = joinFields.at( idx );
        f.setName( prefix + f.name() );
        fields.append( f, QgsFields::OriginJoin, idx + ( joinIdx*1000 ) );
      }
    }
  }
}

void QgsVectorLayerJoinBuffer::createJoinCaches()
{
  QMutexLocker locker( &mMutex );
  QList< QgsVectorJoinInfo >::iterator joinIt = mVectorJoins.begin();
  for ( ; joinIt != mVectorJoins.end(); ++joinIt )
  {
    if ( joinIt->memoryCache && joinIt->cacheDirty )
      cacheJoinLayer( *joinIt );

    // make sure we are connected to the joined layer
    if ( QgsVectorLayer* vl = qobject_cast<QgsVectorLayer*>( QgsProject::instance()->mapLayer( joinIt->joinLayerId ) ) )
    {
      connect( vl, &QgsVectorLayer::updatedFields, this, &QgsVectorLayerJoinBuffer::joinedLayerUpdatedFields, Qt::UniqueConnection );
      connect( vl, &QgsVectorLayer::layerModified, this, &QgsVectorLayerJoinBuffer::joinedLayerModified, Qt::UniqueConnection );
    }
  }
}


void QgsVectorLayerJoinBuffer::writeXml( QDomNode& layer_node, QDomDocument& document ) const
{
  QDomElement vectorJoinsElem = document.createElement( QStringLiteral( "vectorjoins" ) );
  layer_node.appendChild( vectorJoinsElem );
  QList< QgsVectorJoinInfo >::const_iterator joinIt = mVectorJoins.constBegin();
  for ( ; joinIt != mVectorJoins.constEnd(); ++joinIt )
  {
    QDomElement joinElem = document.createElement( QStringLiteral( "join" ) );

    if ( joinIt->targetFieldName.isEmpty() )
      joinElem.setAttribute( QStringLiteral( "targetField" ), joinIt->targetFieldIndex ); //for compatibility with 1.x
    else
      joinElem.setAttribute( QStringLiteral( "targetFieldName" ), joinIt->targetFieldName );

    joinElem.setAttribute( QStringLiteral( "joinLayerId" ), joinIt->joinLayerId );
    if ( joinIt->joinFieldName.isEmpty() )
      joinElem.setAttribute( QStringLiteral( "joinField" ), joinIt->joinFieldIndex ); //for compatibility with 1.x
    else
      joinElem.setAttribute( QStringLiteral( "joinFieldName" ), joinIt->joinFieldName );

    joinElem.setAttribute( QStringLiteral( "memoryCache" ), joinIt->memoryCache );

    if ( joinIt->joinFieldNamesSubset() )
    {
      QDomElement subsetElem = document.createElement( QStringLiteral( "joinFieldsSubset" ) );
      Q_FOREACH ( const QString& fieldName, *joinIt->joinFieldNamesSubset() )
      {
        QDomElement fieldElem = document.createElement( QStringLiteral( "field" ) );
        fieldElem.setAttribute( QStringLiteral( "name" ), fieldName );
        subsetElem.appendChild( fieldElem );
      }

      joinElem.appendChild( subsetElem );
    }

    if ( !joinIt->prefix.isNull() )
    {
      joinElem.setAttribute( QStringLiteral( "customPrefix" ), joinIt->prefix );
      joinElem.setAttribute( QStringLiteral( "hasCustomPrefix" ), 1 );
    }

    vectorJoinsElem.appendChild( joinElem );
  }
}

void QgsVectorLayerJoinBuffer::readXml( const QDomNode& layer_node )
{
  mVectorJoins.clear();
  QDomElement vectorJoinsElem = layer_node.firstChildElement( QStringLiteral( "vectorjoins" ) );
  if ( !vectorJoinsElem.isNull() )
  {
    QDomNodeList joinList = vectorJoinsElem.elementsByTagName( QStringLiteral( "join" ) );
    for ( int i = 0; i < joinList.size(); ++i )
    {
      QDomElement infoElem = joinList.at( i ).toElement();
      QgsVectorJoinInfo info;
      info.joinFieldName = infoElem.attribute( QStringLiteral( "joinFieldName" ) );
      info.joinLayerId = infoElem.attribute( QStringLiteral( "joinLayerId" ) );
      info.targetFieldName = infoElem.attribute( QStringLiteral( "targetFieldName" ) );
      info.memoryCache = infoElem.attribute( QStringLiteral( "memoryCache" ) ).toInt();
      info.cacheDirty = true;

      info.joinFieldIndex = infoElem.attribute( QStringLiteral( "joinField" ) ).toInt(); //for compatibility with 1.x
      info.targetFieldIndex = infoElem.attribute( QStringLiteral( "targetField" ) ).toInt(); //for compatibility with 1.x

      QDomElement subsetElem = infoElem.firstChildElement( QStringLiteral( "joinFieldsSubset" ) );
      if ( !subsetElem.isNull() )
      {
        QStringList* fieldNames = new QStringList;
        QDomNodeList fieldNodes = infoElem.elementsByTagName( QStringLiteral( "field" ) );
        fieldNames->reserve( fieldNodes.count() );
        for ( int i = 0; i < fieldNodes.count(); ++i )
          *fieldNames << fieldNodes.at( i ).toElement().attribute( QStringLiteral( "name" ) );
        info.setJoinFieldNamesSubset( fieldNames );
      }

      if ( infoElem.attribute( QStringLiteral( "hasCustomPrefix" ) ).toInt() )
        info.prefix = infoElem.attribute( QStringLiteral( "customPrefix" ) );
      else
        info.prefix = QString::null;

      addJoin( info );
    }
  }
}

int QgsVectorLayerJoinBuffer::joinedFieldsOffset( const QgsVectorJoinInfo* info, const QgsFields& fields )
{
  if ( !info )
    return -1;

  int joinIndex = mVectorJoins.indexOf( *info );
  if ( joinIndex == -1 )
    return -1;

  for ( int i = 0; i < fields.count(); ++i )
  {
    if ( fields.fieldOrigin( i ) != QgsFields::OriginJoin )
      continue;

    if ( fields.fieldOriginIndex( i ) / 1000 == joinIndex )
      return i;
  }
  return -1;
}

const QgsVectorJoinInfo* QgsVectorLayerJoinBuffer::joinForFieldIndex( int index, const QgsFields& fields, int& sourceFieldIndex ) const
{
  if ( fields.fieldOrigin( index ) != QgsFields::OriginJoin )
    return nullptr;

  int originIndex = fields.fieldOriginIndex( index );
  int sourceJoinIndex = originIndex / 1000;
  sourceFieldIndex = originIndex % 1000;

  if ( sourceJoinIndex < 0 || sourceJoinIndex >= mVectorJoins.count() )
    return nullptr;

  return &( mVectorJoins[sourceJoinIndex] );
}

QgsVectorLayerJoinBuffer* QgsVectorLayerJoinBuffer::clone() const
{
  QgsVectorLayerJoinBuffer* cloned = new QgsVectorLayerJoinBuffer( mLayer );
  cloned->mVectorJoins = mVectorJoins;
  return cloned;
}

void QgsVectorLayerJoinBuffer::joinedLayerUpdatedFields()
{
  // TODO - check - this whole method is probably not needed anymore,
  // since the cache handling is covered by joinedLayerModified()

  QgsVectorLayer* joinedLayer = qobject_cast<QgsVectorLayer*>( sender() );
  Q_ASSERT( joinedLayer );

  // recache the joined layer
  for ( QgsVectorJoinList::iterator it = mVectorJoins.begin(); it != mVectorJoins.end(); ++it )
  {
    if ( joinedLayer->id() == it->joinLayerId )
    {
      it->cachedAttributes.clear();
      cacheJoinLayer( *it );
    }
  }

  emit joinedFieldsChanged();
}

void QgsVectorLayerJoinBuffer::joinedLayerModified()
{
  QgsVectorLayer* joinedLayer = qobject_cast<QgsVectorLayer*>( sender() );
  Q_ASSERT( joinedLayer );

  // recache the joined layer
  for ( QgsVectorJoinList::iterator it = mVectorJoins.begin(); it != mVectorJoins.end(); ++it )
  {
    if ( joinedLayer->id() == it->joinLayerId )
    {
      it->cacheDirty = true;
    }
  }
}
