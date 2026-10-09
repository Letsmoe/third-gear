#include "ExactFloatingPoint.h"
#include "Earcut.h"

#include <algorithm>
#include <deque>

namespace WorldBuilder
{
namespace
{
/** One vertex of the circular doubly linked list the algorithm cuts ears from. */
struct FNode
{
	uint32_t Index = 0;
	double X = 0.0;
	double Y = 0.0;
	FNode* Previous = nullptr;
	FNode* Next = nullptr;
};

/** Signed area of the triangle p, q, r (twice, negative when counter-clockwise in this algorithm's convention). */
double TriangleArea(const FNode* P, const FNode* Q, const FNode* R)
{
	return (Q->Y - P->Y) * (R->X - Q->X) - (Q->X - P->X) * (R->Y - Q->Y);
}

/** True when both nodes are at the same place. */
bool SamePosition(const FNode* First, const FNode* Second)
{
	return First->X == Second->X && First->Y == Second->Y;
}

/** The sign of the value: -1, 0 or 1. */
int SignOf(double Value)
{
	if (Value > 0.0)
	{
		return 1;
	}
	return Value < 0.0 ? -1 : 0;
}

/** For collinear points p, q, r: whether q lies on the segment p-r. */
bool OnSegment(const FNode* P, const FNode* Q, const FNode* R)
{
	return Q->X <= std::max(P->X, R->X) && Q->X >= std::min(P->X, R->X) && Q->Y <= std::max(P->Y, R->Y)
		&& Q->Y >= std::min(P->Y, R->Y);
}

/** Whether the segments p1-q1 and p2-q2 intersect. */
bool SegmentsIntersect(const FNode* P1, const FNode* Q1, const FNode* P2, const FNode* Q2)
{
	const int First = SignOf(TriangleArea(P1, Q1, P2));
	const int Second = SignOf(TriangleArea(P1, Q1, Q2));
	const int Third = SignOf(TriangleArea(P2, Q2, P1));
	const int Fourth = SignOf(TriangleArea(P2, Q2, Q1));
	if (First != Second && Third != Fourth)
	{
		return true;
	}
	if (First == 0 && OnSegment(P1, P2, Q1))
	{
		return true;
	}
	if (Second == 0 && OnSegment(P1, Q2, Q1))
	{
		return true;
	}
	if (Third == 0 && OnSegment(P2, P1, Q2))
	{
		return true;
	}
	return Fourth == 0 && OnSegment(P2, Q1, Q2);
}

/** Whether a point lies within a convex triangle. */
bool PointInTriangle(double AX, double AY, double BX, double BY, double CX, double CY, double PX, double PY)
{
	return (CX - PX) * (AY - PY) >= (AX - PX) * (CY - PY) && (AX - PX) * (BY - PY) >= (BX - PX) * (AY - PY)
		&& (BX - PX) * (CY - PY) >= (CX - PX) * (BY - PY);
}

/** Like PointInTriangle, but false for the triangle's first corner. */
bool PointInTriangleExceptFirst(double AX, double AY, double BX, double BY, double CX, double CY, double PX, double PY)
{
	return !(AX == PX && AY == PY) && PointInTriangle(AX, AY, BX, BY, CX, CY, PX, PY);
}

/** Whether the diagonal from a lies inside the polygon at a. */
bool LocallyInside(const FNode* A, const FNode* B)
{
	if (TriangleArea(A->Previous, A, A->Next) < 0.0)
	{
		return TriangleArea(A, B, A->Next) >= 0.0 && TriangleArea(A, A->Previous, B) >= 0.0;
	}
	return TriangleArea(A, B, A->Previous) < 0.0 || TriangleArea(A, A->Next, B) < 0.0;
}

/** Whether the middle of the diagonal a-b is inside the polygon. */
bool MiddleInside(const FNode* A, const FNode* B)
{
	const FNode* P = A;
	bool bInside = false;
	const double PX = (A->X + B->X) / 2;
	const double PY = (A->Y + B->Y) / 2;
	do
	{
		if (((P->Y > PY) != (P->Next->Y > PY)) && P->Next->Y != P->Y
			&& (PX < (P->Next->X - P->X) * (PY - P->Y) / (P->Next->Y - P->Y) + P->X))
		{
			bInside = !bInside;
		}
		P = P->Next;
	} while (P != A);
	return bInside;
}

/** Whether the diagonal a-b crosses an edge of the polygon. */
bool IntersectsPolygon(const FNode* A, const FNode* B)
{
	const FNode* P = A;
	do
	{
		if (P->Index != A->Index && P->Next->Index != A->Index && P->Index != B->Index && P->Next->Index != B->Index
			&& SegmentsIntersect(P, P->Next, A, B))
		{
			return true;
		}
		P = P->Next;
	} while (P != A);
	return false;
}

/** Whether a-b is a diagonal that lies in the polygon's interior. */
bool IsValidDiagonal(const FNode* A, const FNode* B)
{
	const bool bInterior = A->Next->Index != B->Index && A->Previous->Index != B->Index && !IntersectsPolygon(A, B)
		&& ((LocallyInside(A, B) && LocallyInside(B, A) && MiddleInside(A, B)
				&& (TriangleArea(A->Previous, A, B->Previous) != 0.0 || TriangleArea(A, B->Previous, B) != 0.0))
			|| (SamePosition(A, B) && TriangleArea(A->Previous, A, A->Next) > 0.0 && TriangleArea(B->Previous, B, B->Next) > 0.0));
	return bInterior;
}

/** Triangulation state: the node pool and the output. */
class FEarcut
{
public:
	/** Triangulates the ring; empty when it has fewer than three distinct points. */
	std::vector<uint32_t> Run(const std::vector<FWorldPoint>& Ring)
	{
		FNode* Outer = LinkedList(Ring);
		if (Outer == nullptr || Outer->Next == Outer->Previous)
		{
			return {};
		}
		EarcutLinked(Outer, 0);
		return Triangles;
	}

private:
	std::deque<FNode> Pool;
	std::vector<uint32_t> Triangles;

	/** Adds an unlinked node to the pool. */
	FNode* CreateNode(uint32_t Index, double X, double Y)
	{
		FNode& Node = Pool.emplace_back();
		Node.Index = Index;
		Node.X = X;
		Node.Y = Y;
		return &Node;
	}

	/** Adds a node after Last, or starts a new list. */
	FNode* InsertNode(uint32_t Index, double X, double Y, FNode* Last)
	{
		FNode* Node = CreateNode(Index, X, Y);
		if (Last == nullptr)
		{
			Node->Previous = Node;
			Node->Next = Node;
			return Node;
		}
		Node->Next = Last->Next;
		Node->Previous = Last;
		Last->Next->Previous = Node;
		Last->Next = Node;
		return Node;
	}

	/** Takes the node out of its list. */
	static void RemoveNode(FNode* Node)
	{
		Node->Next->Previous = Node->Previous;
		Node->Previous->Next = Node->Next;
	}

	/** The circular list of the ring's points in clockwise order (in this algorithm's convention). */
	FNode* LinkedList(const std::vector<FWorldPoint>& Ring)
	{
		double Sum = 0.0;
		for (size_t Index = 0, Previous = Ring.size() - 1; Index < Ring.size(); Previous = Index++)
		{
			Sum += (Ring[Previous].X - Ring[Index].X) * (Ring[Index].Y + Ring[Previous].Y);
		}
		FNode* Last = nullptr;
		if (Sum > 0.0)
		{
			for (size_t Index = 0; Index < Ring.size(); ++Index)
			{
				Last = InsertNode(static_cast<uint32_t>(Index), Ring[Index].X, Ring[Index].Y, Last);
			}
		}
		else
		{
			for (size_t Index = Ring.size(); Index-- > 0;)
			{
				Last = InsertNode(static_cast<uint32_t>(Index), Ring[Index].X, Ring[Index].Y, Last);
			}
		}
		if (Last != nullptr && SamePosition(Last, Last->Next))
		{
			RemoveNode(Last);
			Last = Last->Next;
		}
		return Last;
	}

	/** Removes duplicate and collinear points; returns a node still in the list. */
	FNode* FilterPoints(FNode* Start, FNode* End = nullptr)
	{
		if (Start == nullptr)
		{
			return Start;
		}
		if (End == nullptr)
		{
			End = Start;
		}
		FNode* P = Start;
		bool bAgain;
		do
		{
			bAgain = false;
			if (SamePosition(P, P->Next) || TriangleArea(P->Previous, P, P->Next) == 0.0)
			{
				RemoveNode(P);
				P = End = P->Previous;
				if (P == P->Next)
				{
					break;
				}
				bAgain = true;
			}
			else
			{
				P = P->Next;
			}
		} while (bAgain || P != End);
		return End;
	}

	/** Whether the node and its neighbours form an ear with no other point inside. */
	static bool IsEar(const FNode* Ear)
	{
		const FNode* A = Ear->Previous;
		const FNode* B = Ear;
		const FNode* C = Ear->Next;
		if (TriangleArea(A, B, C) >= 0.0)
		{
			return false; // reflex, can't be an ear
		}
		const double MinX = std::min({A->X, B->X, C->X});
		const double MinY = std::min({A->Y, B->Y, C->Y});
		const double MaxX = std::max({A->X, B->X, C->X});
		const double MaxY = std::max({A->Y, B->Y, C->Y});
		const FNode* P = C->Next;
		while (P != A)
		{
			if (P->X >= MinX && P->X <= MaxX && P->Y >= MinY && P->Y <= MaxY
				&& PointInTriangleExceptFirst(A->X, A->Y, B->X, B->Y, C->X, C->Y, P->X, P->Y)
				&& TriangleArea(P->Previous, P, P->Next) >= 0.0)
			{
				return false;
			}
			P = P->Next;
		}
		return true;
	}

	/** The repair that follows when no ear can be cut: Pass 0 filters points, 1 cures small intersections, 2 splits. */
	void RepairStall(FNode* Ear, int Pass)
	{
		if (Pass == 0)
		{
			EarcutLinked(FilterPoints(Ear), 1);
		}
		else if (Pass == 1)
		{
			Ear = CureLocalIntersections(FilterPoints(Ear));
			EarcutLinked(Ear, 2);
		}
		else if (Pass == 2)
		{
			SplitEarcut(Ear);
		}
	}

	/** The main loop: cuts ears one by one, retrying with the repairs in Pass 1 and 2 when stuck. */
	void EarcutLinked(FNode* Ear, int Pass)
	{
		if (Ear == nullptr)
		{
			return;
		}
		FNode* Stop = Ear;
		while (Ear->Previous != Ear->Next)
		{
			FNode* Previous = Ear->Previous;
			FNode* Next = Ear->Next;
			if (IsEar(Ear))
			{
				Triangles.push_back(Previous->Index);
				Triangles.push_back(Ear->Index);
				Triangles.push_back(Next->Index);
				RemoveNode(Ear);
				// Skipping the next vertex leads to fewer sliver triangles.
				Ear = Next->Next;
				Stop = Next->Next;
				continue;
			}
			Ear = Next;
			if (Ear == Stop)
			{
				RepairStall(Ear, Pass);
				break;
			}
		}
	}

	/** Cuts small local self-intersections off the list. */
	FNode* CureLocalIntersections(FNode* Start)
	{
		FNode* P = Start;
		do
		{
			FNode* A = P->Previous;
			FNode* B = P->Next->Next;
			if (!SamePosition(A, B) && SegmentsIntersect(A, P, P->Next, B) && LocallyInside(A, B) && LocallyInside(B, A))
			{
				Triangles.push_back(A->Index);
				Triangles.push_back(P->Index);
				Triangles.push_back(B->Index);
				RemoveNode(P);
				RemoveNode(P->Next);
				P = Start = B;
			}
			P = P->Next;
		} while (P != Start);
		return FilterPoints(P);
	}

	/** Splits the polygon along a diagonal; returns the node that starts the second half. */
	FNode* SplitPolygon(FNode* A, FNode* B)
	{
		FNode* A2 = CreateNode(A->Index, A->X, A->Y);
		FNode* B2 = CreateNode(B->Index, B->X, B->Y);
		FNode* AfterA = A->Next;
		FNode* BeforeB = B->Previous;
		A->Next = B;
		B->Previous = A;
		A2->Next = AfterA;
		AfterA->Previous = A2;
		B2->Next = A2;
		A2->Previous = B2;
		BeforeB->Next = B2;
		B2->Previous = BeforeB;
		return B2;
	}

	/** Splits the polygon along the diagonal from A to B and triangulates the two halves on their own. */
	void SplitAlongDiagonal(FNode* A, FNode* B)
	{
		FNode* C = SplitPolygon(A, B);
		A = FilterPoints(A, A->Next);
		C = FilterPoints(C, C->Next);
		EarcutLinked(A, 0);
		EarcutLinked(C, 0);
	}

	/** Last resort: finds a valid diagonal, splits the polygon in two and triangulates the halves on their own. */
	void SplitEarcut(FNode* Start)
	{
		FNode* A = Start;
		do
		{
			for (FNode* B = A->Next->Next; B != A->Previous; B = B->Next)
			{
				if (A->Index != B->Index && IsValidDiagonal(A, B))
				{
					SplitAlongDiagonal(A, B);
					return;
				}
			}
			A = A->Next;
		} while (A != Start);
	}
};
}

std::vector<uint32_t> TriangulateRing(const std::vector<FWorldPoint>& Ring)
{
	if (Ring.size() < 3)
	{
		return {};
	}
	FEarcut Earcut;
	return Earcut.Run(Ring);
}
}
