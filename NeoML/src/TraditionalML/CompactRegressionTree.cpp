/* Copyright © 2017-2020 ABBYY Production LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
--------------------------------------------------------------------------------------------------------------*/

#include <common.h>
#pragma hdrstop

#include <CompactRegressionTree.h>
#include <SerializeCompact.h>

namespace NeoML {

/////////////////////////////////////////////////////////////////////////////////////////

REGISTER_NEOML_MODEL( CCompact16RegressionTree, "CompactRegressionTree" )
REGISTER_NEOML_MODEL( CCompact32RegressionTree, "Compact32RegressionTree" )

/////////////////////////////////////////////////////////////////////////////////////////

template<class T>
CCompactRegressionTree<T>::CCompactRegressionTree(
	const NeoML::IRegressionTreeNode* source )
{
	importNodes( source );
}

// Recursively converts IRegressionTreeNode into CNode-s.
template<class T>
void CCompactRegressionTree<T>::importNodes(
	const NeoML::IRegressionTreeNode* source )
{
	NeoAssert( source != 0 );

	CRegressionTreeNodeInfo info;
	source->GetNodeInfo( info );

	const int index = nodes.Size();
	CNode& node = nodes.Append();

	switch( info.Type ) {
		case NeoML::TRegressionTreeNodeType::RTNT_Const:
		case NeoML::TRegressionTreeNodeType::RTNT_MultiConst:
			node.FeaturePlusOne = 0;
			if( predictionSize == NotFound ) {
				predictionSize = info.Value.Size();
			} else {
				NeoAssert( predictionSize == info.Value.Size() );
			}
			NeoAssert( predictionSize >= 1 );

			if( predictionSize == 1 ) {
				node.Value.Resident = static_cast<float>( info.Value[0] );
			} else {
				node.Value.NonresidentIndex = nonresidentValues.Size();
				for( int i = 0; i < info.Value.Size(); i++ ) {
					nonresidentValues.Add( static_cast<float>( info.Value[i] ) );
				}
			}
			break;

		case NeoML::TRegressionTreeNodeType::RTNT_Continuous:
			NeoAssert( static_cast<uint64_t>( info.FeatureIndex ) <= MaxFeature );
			node.FeaturePlusOne = static_cast<T>( info.FeatureIndex + 1 );

			NeoAssert( info.Value.Size() == 1 );
			node.Value.Resident = static_cast<float>( info.Value[0] );

			importNodes( source->GetLeftChild() );
			NeoAssert( static_cast<uint64_t>( nodes.Size() ) <= MaxNodeIndex );
			// NB: `nodes[index]` instead of `node` because of possible reallocation.
			nodes[index].RightChildIndex = static_cast<uint16_t>( nodes.Size() );
			importNodes( source->GetRightChild() );
			break;

		default:
			NeoAssert( false );
	}
}

// Actual implementation of IRegressionTreeNode for this class and CNodeWrapper,
template<class T>
CPtr<const IRegressionTreeNode> CCompactRegressionTree<T>::GetLeftChild(
	int nodeIndex ) const
{
	NeoAssert( nodes.IsValidIndex( nodeIndex ) );
	if( nodes[nodeIndex].FeaturePlusOne == 0 ) {
		return nullptr;
	}
	return getWrapper( nodeIndex + 1 );
}

template<class T>
CPtr<const IRegressionTreeNode> CCompactRegressionTree<T>::GetRightChild(
	int nodeIndex ) const
{
	NeoAssert( nodes.IsValidIndex( nodeIndex ) );
	if( nodes[nodeIndex].FeaturePlusOne == 0 ) {
		return nullptr;
	}
	return getWrapper( nodes[nodeIndex].RightChildIndex );
}

// Returns wrapper for agiven node.
template<class T>
CPtr<const IRegressionTreeNode> CCompactRegressionTree<T>::getWrapper( int nodeIndex ) const
{
	NeoAssert( nodes.IsValidIndex( nodeIndex ) );
	if( nodeIndex == 0 ) {
		return this;
	}

	wrappers.SetSize( nodes.Size() );
	if( wrappers[nodeIndex] == 0 ) {
		wrappers[nodeIndex] = FINE_DEBUG_NEW CNodeWrapper( *this, nodeIndex );
	}
	return wrappers[nodeIndex];
}

template<class T>
void CCompactRegressionTree<T>::GetNodeInfo(
	int nodeIndex , CRegressionTreeNodeInfo& info ) const
{
	NeoAssert( nodes.IsValidIndex( nodeIndex ) );

	const CNode& node = nodes[nodeIndex];
	if( node.FeaturePlusOne != 0 ) {
		info.Type = RTNT_Continuous;
		info.FeatureIndex = node.FeaturePlusOne - 1;
		info.Value.SetSize(1);
		info.Value[0] = node.Value.Resident;
		return;
	}

	info.FeatureIndex = NotFound;
	info.Value.SetSize( predictionSize );

	if( predictionSize == 1 ) {
		info.Type = RTNT_Const;
		info.Value[0] = node.Value.Resident;
	} else {
		info.Type = RTNT_MultiConst;
		for( int i = 0; i < predictionSize; i++ ) {
			info.Value[i] = nonresidentValues[node.Value.NonresidentIndex + i];
		}
	}
}

template<typename TVector>
static inline float getFeature( const TVector& features, int number )
{
	return features[number];
}

static inline float getFeature( const CFloatVectorDesc& features, int number )
{
	return GetValue( features, number );
}

template<class T>
template<typename TVector>
inline const float* CCompactRegressionTree<T>::predict( const TVector& features ) const
{
	int index = 0;
	for( ;; ) {
		const CNode& node = nodes[index];
		if( node.FeaturePlusOne != 0 ) {
			const float featureValue = getFeature( features, node.FeaturePlusOne - 1 );
			if( featureValue <= node.Value.Resident ) {
				index++;
			} else {
				index = node.RightChildIndex;
			}
		} else if( predictionSize == 1 ) {
			return &node.Value.Resident;
		} else {
			return &nonresidentValues[node.Value.NonresidentIndex];
		}
	}
	NeoAssert( false );
	return 0;
}

template<class T>
template<typename TVector>
inline void CCompactRegressionTree<T>::predict(
	const TVector& features, CPrediction& result ) const
{
	const float* pValues = predict( features );
	result.SetSize( predictionSize );
	for( int i = 0; i < predictionSize; i++ ) {
		result[i] = pValues[i];
	}
}

// CRegressionTree methods implementation.
template<class T>
void CCompactRegressionTree<T>::Predict(
	const CFloatVector& features, CPrediction& result ) const
{
	predict( features.GetPtr(), result );
}

template<class T>
void CCompactRegressionTree<T>::Predict(
	const CFloatVectorDesc& features, CPrediction& result ) const
{
	predict( features, result );
}

template<class T>
double CCompactRegressionTree<T>::Predict( const CFloatVector& features ) const
{
	NeoPresume( predictionSize == 1 );
	return *predict( features.GetPtr() );
}

template<class T>
double CCompactRegressionTree<T>::Predict( const CFloatVectorDesc& features ) const
{
	NeoPresume( predictionSize == 1 );
	return *predict( features );
}

template<class T>
void CCompactRegressionTree<T>::CalcFeatureStatistics(
	int maxFeature, CArray<int>& result ) const
{
	result.DeleteAll();
	result.Add( 0, maxFeature );

	for( int i = 0; i < nodes.Size(); i++ ) {
		const CNode& node = nodes[i];
		if( node.FeaturePlusOne != 0 && node.FeaturePlusOne <= static_cast<uint64_t>( maxFeature ) ) {
			result[node.FeaturePlusOne - 1]++;
		}
	}
}

template<class T>
void CCompactRegressionTree<T>::Serialize( CArchive& archive )
{
	/*const int version =*/ archive.SerializeVersion(0);

	archive.SerializeSmallValue( predictionSize );

	int nodesCount = nodes.Size();
	SerializeCompact( archive, nodesCount );
	if( archive.IsLoading() ) {
		check( ( nodesCount == 0 && predictionSize == NotFound ) ||
				( nodesCount >= 0 && static_cast<uint64_t>( nodesCount ) <= MaxNodeIndex && predictionSize >= 1 ),
			ERR_BAD_ARCHIVE, archive.Name() );
		nodes.SetSize( nodesCount );
		wrappers.DeleteAll();
	}

	CFastArray<int, 32> parents{ -1 };
	for( int i = 0; i < nodes.Size(); i++ ) {
		CNode& node = nodes[i];
		if( archive.IsLoading() ) {
			const int lastParent = parents.Last();
			if( i - lastParent > 1 ) {
				nodes[lastParent].RightChildIndex = i;
				parents.DeleteLast();
			}
		}

		SerializeCompact( archive, node.FeaturePlusOne );
		if( node.FeaturePlusOne != 0 ) {
			SerializeCompact( archive, node.Value.Resident );
			if( archive.IsLoading() ) {
				parents.Add(i);
			}
		} else if( predictionSize == 1 ) {
			archive.Serialize( node.Value.Resident );
		} else {
			SerializeCompact( archive, node.Value.NonresidentIndex );
		}
	}

	nonresidentValues.Serialize( archive );
	if( archive.IsLoading() ) {
		check( nonresidentValues.IsEmpty() ||
				( predictionSize > 1 && nonresidentValues.Size() % predictionSize == 0 ),
			ERR_BAD_ARCHIVE, archive.Name() );
	}
}

template class CCompactRegressionTree<uint16_t>;
template class CCompactRegressionTree<uint32_t>;

/////////////////////////////////////////////////////////////////////////////////////////

} // namespace NeoML

