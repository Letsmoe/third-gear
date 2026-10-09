#include "ExactFloatingPoint.h"
#include "Skeleton.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>

namespace WorldBuilder
{
namespace
{
constexpr double Epsilon = 1e-9;
constexpr double TimeEpsilon = 1e-7;
constexpr double PositionTolerance = 1e-6;

using FVector = FWorldPoint;

/** The sum of two vectors. */
FVector Add(const FVector& A, const FVector& B)
{
	return {A.X + B.X, A.Y + B.Y};
}

/** The vector from B to A. */
FVector Subtract(const FVector& A, const FVector& B)
{
	return {A.X - B.X, A.Y - B.Y};
}

/** The vector times a factor. */
FVector Scale(const FVector& A, double Factor)
{
	return {A.X * Factor, A.Y * Factor};
}

/** Dot product rounded like numpy's: the second product is fused into the sum. */
double Dot(const FVector& A, const FVector& B)
{
	return std::fma(A.Y, B.Y, A.X * B.X);
}

/** A line moving inward: points x with normal . x = offset + time. */
struct FEdge
{
	int Original = 0;
	FVector Direction;
	FVector Normal;
	double Offset = 0.0;
};

/** The part of an edge between two wavefront vertices, with the nodes its two end vertices have passed. */
struct FInstance
{
	const FEdge* Edge = nullptr;
	std::vector<int> TailChain;
	std::vector<int> HeadChain;
};

struct FVertex
{
	const FEdge* InEdge = nullptr;
	const FEdge* OutEdge = nullptr;
	int BirthNode = 0;
	double BirthTime = 0.0;
	FVertex* Previous = nullptr;
	FVertex* Next = nullptr;
	FInstance* OutInstance = nullptr;
	bool bAlive = true;
	bool bReflex = false;
	FVector Base;
	FVector Velocity;

	FVector Position(double Time) const { return Add(Base, Scale(Velocity, Time)); }
};

/** The kinds of event the wavefront has. */
enum class EEventKind
{
	Edge,
	Split,
};

struct FEvent
{
	double Time = 0.0;
	EEventKind Kind = EEventKind::Edge;
	FVertex* First = nullptr;
	FVertex* Second = nullptr;
};

/** The vector with length 1; unchanged when it is too short. */
FVector Unit(const FVector& Vector)
{
	const double Length = std::hypot(Vector.X, Vector.Y);
	if (Length > Epsilon)
	{
		return {Vector.X / Length, Vector.Y / Length};
	}
	return Vector;
}

/**
 * Solves the 2x2 system by LU decomposition with partial pivoting, rounded exactly like numpy's solve (LAPACK
 * gesv in OpenBLAS): the multiplier uses the reciprocal of the pivot, and two of the updates are fused.
 * The skeleton compares event times that tie in exact arithmetic, so the last bit decides which event comes first.
 */
void SolveTwoByTwo(const FVector& Row0, const FVector& Row1, double Right0, double Right1, FVector& Solution)
{
	double A00 = Row0.X, A01 = Row0.Y, A10 = Row1.X, A11 = Row1.Y;
	double B0 = Right0, B1 = Right1;
	if (std::abs(A10) > std::abs(A00))
	{
		std::swap(A00, A10);
		std::swap(A01, A11);
		std::swap(B0, B1);
	}
	if (A00 == 0.0)
	{
		throw std::runtime_error("singular matrix");
	}
	const double Multiplier = A10 * (1.0 / A00);
	const double U11 = A11 - Multiplier * A01;
	if (U11 == 0.0)
	{
		throw std::runtime_error("singular matrix");
	}
	const double Y1 = std::fma(-Multiplier, B0, B1);
	Solution.Y = Y1 / U11;
	Solution.X = std::fma(-A01, Solution.Y, B0) / A00;
}

/** Base point and velocity of a vertex: position(t) = base + velocity * t for the time its edges stay the same. */
void ComputeMotion(FVertex& Vertex)
{
	const FEdge& First = *Vertex.InEdge;
	const FEdge& Second = *Vertex.OutEdge;
	const double Determinant = First.Normal.X * Second.Normal.Y - First.Normal.Y * Second.Normal.X;
	if (std::abs(Determinant) < 1e-9)
	{
		// Parallel edges: the vertex slides along their common normal from where it was born.
		Vertex.Velocity = First.Normal;
		Vertex.Base = Subtract(Vertex.Base, Scale(Vertex.Velocity, Vertex.BirthTime));
		return;
	}
	FVector Base;
	FVector Velocity;
	SolveTwoByTwo(First.Normal, Second.Normal, First.Offset, Second.Offset, Base);
	SolveTwoByTwo(First.Normal, Second.Normal, 1.0, 1.0, Velocity);
	Vertex.Base = Base;
	Vertex.Velocity = Velocity;
}

/** Everything one skeleton computation owns: vertices, instances and edges live as long as it does. */
class FSkeletonBuilder
{
public:
	explicit FSkeletonBuilder(FSkeleton& InSkeleton) : Skeleton(InSkeleton) {}

	/** Runs the wavefront for the rings; Skeleton.bOk tells whether it worked. */
	void Run(const std::vector<FRing>& Rings)
	{
		size_t Total = 0;
		for (const FRing& Ring : Rings)
		{
			Total += Ring.size();
		}
		if (Total > MaxSkeletonVertices)
		{
			Skeleton.bOk = false;
			return;
		}
		std::vector<FVertex*> Vertices;
		if (!BuildLoops(Rings, Vertices))
		{
			Skeleton.bOk = false;
			return;
		}
		try
		{
			RunEvents(Vertices);
		}
		catch (const std::exception&)
		{
			Skeleton.bOk = false;
		}
	}

private:
	FSkeleton& Skeleton;
	std::deque<FEdge> Edges;
	std::deque<FVertex> VertexPool;
	std::deque<FInstance> InstancePool;

	/** Adds a skeleton node and returns its index. */
	int AddNode(double X, double Y, double Time)
	{
		Skeleton.Nodes.push_back({X, Y, Time});
		return static_cast<int>(Skeleton.Nodes.size()) - 1;
	}

	/** A new stretch of an edge between two nodes. */
	FInstance* NewInstance(const FEdge* Edge, int TailNode, int HeadNode)
	{
		FInstance& Instance = InstancePool.emplace_back();
		Instance.Edge = Edge;
		Instance.TailChain = {TailNode};
		Instance.HeadChain = {HeadNode};
		return &Instance;
	}

	/** Creates a wavefront vertex where two edges meet and works out its motion. */
	FVertex* MakeVertex(const FEdge* InEdge, const FEdge* OutEdge, const FVector& Position, double Time, int BirthNode)
	{
		FVertex& Vertex = VertexPool.emplace_back();
		Vertex.InEdge = InEdge;
		Vertex.OutEdge = OutEdge;
		Vertex.BirthNode = BirthNode;
		Vertex.BirthTime = Time;
		const double Cross = InEdge->Direction.X * OutEdge->Direction.Y - InEdge->Direction.Y * OutEdge->Direction.X;
		Vertex.bReflex = Cross < -1e-9;
		Vertex.Base = Position;
		ComputeMotion(Vertex);
		return &Vertex;
	}

	/** The index of the first point where the outline does not turn, or -1. */
	static int FindStraightPoint(const std::vector<FVector>& Points)
	{
		for (size_t Index = 0; Index < Points.size(); ++Index)
		{
			const FVector& Before = Points[(Index + Points.size() - 1) % Points.size()];
			const FVector& Here = Points[Index];
			const FVector& After = Points[(Index + 1) % Points.size()];
			const FVector First = Unit(Subtract(Here, Before));
			const FVector Second = Unit(Subtract(After, Here));
			if (std::abs(First.X * Second.Y - First.Y * Second.X) < 1e-7 && Dot(First, Second) > 0.0)
			{
				return static_cast<int>(Index);
			}
		}
		return -1;
	}

	/** Drops repeated points and vertices where the outline doesn't turn. */
	static std::vector<FVector> RemoveCollinear(const FRing& Ring)
	{
		std::vector<FVector> Points = {Ring[0]};
		for (size_t Index = 1; Index < Ring.size(); ++Index)
		{
			const FVector& Last = Points.back();
			if (std::hypot(Ring[Index].X - Last.X, Ring[Index].Y - Last.Y) > 1e-6)
			{
				Points.push_back(Ring[Index]);
			}
		}
		if (Points.size() > 1 && std::hypot(Points[0].X - Points.back().X, Points[0].Y - Points.back().Y) < 1e-6)
		{
			Points.pop_back();
		}
		while (Points.size() > 3)
		{
			const int Straight = FindStraightPoint(Points);
			if (Straight < 0)
			{
				break;
			}
			Points.erase(Points.begin() + Straight);
		}
		return Points;
	}

	/** Creates the wavefront loops for the rings; false when a ring collapses. */
	bool BuildLoops(const std::vector<FRing>& Rings, std::vector<FVertex*>& Vertices)
	{
		int EdgeIndex = 0;
		for (const FRing& RawRing : Rings)
		{
			const std::vector<FVector> Ring = RemoveCollinear(RawRing);
			const size_t Count = Ring.size();
			if (Count < 3)
			{
				return false;
			}
			std::vector<const FEdge*> RingEdges;
			for (size_t Index = 0; Index < Count; ++Index)
			{
				const FVector& A = Ring[Index];
				const FVector& B = Ring[(Index + 1) % Count];
				FEdge& Edge = Edges.emplace_back();
				Edge.Original = EdgeIndex + static_cast<int>(Index);
				Edge.Direction = Unit(Subtract(B, A));
				Edge.Normal = {-Edge.Direction.Y, Edge.Direction.X};
				Edge.Offset = Dot(Edge.Normal, A);
				RingEdges.push_back(&Edge);
				Skeleton.Edges.push_back({A, Edge.Direction});
			}
			EdgeIndex += static_cast<int>(Count);
			std::vector<FVertex*> Loop;
			for (size_t Index = 0; Index < Count; ++Index)
			{
				const int Node = AddNode(Ring[Index].X, Ring[Index].Y, 0.0);
				Loop.push_back(MakeVertex(RingEdges[(Index + Count - 1) % Count], RingEdges[Index], Ring[Index], 0.0, Node));
			}
			for (size_t Index = 0; Index < Count; ++Index)
			{
				Loop[Index]->Previous = Loop[(Index + Count - 1) % Count];
				Loop[Index]->Next = Loop[(Index + 1) % Count];
			}
			for (size_t Index = 0; Index < Count; ++Index)
			{
				Loop[Index]->OutInstance = NewInstance(RingEdges[Index], Loop[Index]->BirthNode, Loop[(Index + 1) % Count]->BirthNode);
			}
			Vertices.insert(Vertices.end(), Loop.begin(), Loop.end());
		}
		return true;
	}

	/** The vertices of the wavefront loop that Start is on. */
	static std::vector<FVertex*> LoopVertices(FVertex* Start)
	{
		std::vector<FVertex*> Result = {Start};
		FVertex* Current = Start->Next;
		while (Current != Start)
		{
			Result.push_back(Current);
			Current = Current->Next;
			if (Result.size() > 10000)
			{
				throw std::runtime_error("broken wavefront loop");
			}
		}
		return Result;
	}

	/** When the edge from a vertex to its successor has shrunk to nothing. */
	static std::optional<double> EdgeCollapseTime(const FVertex& Vertex, const FVertex& Successor, double Now)
	{
		const FVector& Direction = Vertex.OutEdge->Direction;
		const double StartGap = Dot(Direction, Subtract(Successor.Base, Vertex.Base));
		const double Rate = Dot(Direction, Subtract(Successor.Velocity, Vertex.Velocity));
		if (Rate >= -1e-12)
		{
			return std::nullopt; // not shrinking
		}
		const double Time = -StartGap / Rate;
		if (Time >= Now - TimeEpsilon)
		{
			return Time;
		}
		return std::nullopt;
	}

	/** Earliest wavefront edge the reflex vertex runs into: (time, tail vertex of that edge). */
	static std::optional<std::pair<double, FVertex*>> SplitCandidate(const FVertex& Vertex, const std::vector<FVertex*>& Alive,
		double Now)
	{
		std::optional<std::pair<double, FVertex*>> Best;
		for (FVertex* Tail : Alive)
		{
			const FEdge* Edge = Tail->OutEdge;
			if (Edge == Vertex.InEdge || Edge == Vertex.OutEdge || Tail == &Vertex)
			{
				continue;
			}
			const FVertex* Head = Tail->Next;
			const double Denominator = Dot(Edge->Normal, Vertex.Velocity) - 1.0;
			if (Denominator > -1e-12)
			{
				continue;
			}
			const double Time = (Edge->Offset - Dot(Edge->Normal, Vertex.Base)) / Denominator;
			if (Time < Now - TimeEpsilon)
			{
				continue;
			}
			const FVector Point = Vertex.Position(Time);
			const FVector TailPoint = Tail->Position(Time);
			const FVector HeadPoint = Head->Position(Time);
			const double Along = Dot(Edge->Direction, Subtract(Point, TailPoint));
			const double Length = Dot(Edge->Direction, Subtract(HeadPoint, TailPoint));
			if (Length < -1e-9 || Along < -1e-7 || Along > Length + 1e-7)
			{
				continue;
			}
			if (!Best.has_value() || Time < Best->first)
			{
				Best = std::make_pair(Time, Tail);
			}
		}
		return Best;
	}

	/** The earliest event; at equal times the first one found wins, and a vertex tries its edge event before its split. */
	static std::optional<FEvent> NextEvent(const std::vector<FVertex*>& Alive, double Now)
	{
		std::optional<FEvent> Best;
		for (FVertex* Vertex : Alive)
		{
			FVertex* Successor = Vertex->Next;
			const std::optional<double> Time = EdgeCollapseTime(*Vertex, *Successor, Now);
			if (Time.has_value() && (!Best.has_value() || *Time < Best->Time))
			{
				Best = FEvent{*Time, EEventKind::Edge, Vertex, Successor};
			}
			if (!Vertex->bReflex)
			{
				continue;
			}
			const auto Hit = SplitCandidate(*Vertex, Alive, Now);
			if (Hit.has_value() && (!Best.has_value() || Hit->first < Best->Time))
			{
				Best = FEvent{Hit->first, EEventKind::Split, Vertex, Hit->second};
			}
		}
		return Best;
	}

	/** True when two nodes are at the same place and time. */
	bool SamePoint(int First, int Second) const
	{
		const FSkeletonNode& A = Skeleton.Nodes[static_cast<size_t>(First)];
		const FSkeletonNode& B = Skeleton.Nodes[static_cast<size_t>(Second)];
		return std::abs(A.X - B.X) < PositionTolerance && std::abs(A.Y - B.Y) < PositionTolerance
			&& std::abs(A.Time - B.Time) < PositionTolerance;
	}

	/** Records a face fragment of the edge, dropping repeated points; fragments under three points are nothing. */
	void AddFragment(const FEdge* Edge, const std::vector<int>& Polygon)
	{
		std::vector<int> Cleaned;
		for (int Node : Polygon)
		{
			if (Cleaned.empty() || !SamePoint(Cleaned.back(), Node))
			{
				Cleaned.push_back(Node);
			}
		}
		while (Cleaned.size() > 1 && SamePoint(Cleaned.front(), Cleaned.back()))
		{
			Cleaned.pop_back();
		}
		if (Cleaned.size() >= 3)
		{
			Skeleton.Faces.push_back({Edge->Original, Cleaned});
		}
	}

	/** Node list of an instance's face fragment: its edge, up the head chain, across, down the tail chain. */
	static std::vector<int> ChainPolygon(const FInstance& Instance)
	{
		std::vector<int> Polygon = {Instance.TailChain.front()};
		Polygon.insert(Polygon.end(), Instance.HeadChain.begin(), Instance.HeadChain.end());
		Polygon.insert(Polygon.end(), Instance.TailChain.rbegin(), Instance.TailChain.rend() - 1);
		return Polygon;
	}

	/** Ends a vertex at a node and records its arc. */
	void Kill(FVertex* Vertex, int Node)
	{
		Vertex->bAlive = false;
		Skeleton.Arcs.push_back({Vertex->BirthNode, Node, Vertex->bReflex});
	}

	/** An edge shrinks to nothing: its two vertices become one. */
	void EdgeEvent(std::vector<FVertex*>& Alive, FVertex* Tail, FVertex* Head, double Time)
	{
		const FVector Point = Add(Scale(Tail->Position(Time), 0.5), Scale(Head->Position(Time), 0.5));
		const int Node = AddNode(Point.X, Point.Y, Time);
		FInstance* Instance = Tail->OutInstance;
		Instance->TailChain.push_back(Node);
		Instance->HeadChain.push_back(Node);
		AddFragment(Instance->Edge, ChainPolygon(*Instance));
		FInstance* BeforeInstance = Tail->Previous->OutInstance;
		BeforeInstance->HeadChain.push_back(Node);
		FInstance* AfterInstance = Head->OutInstance;
		AfterInstance->TailChain.push_back(Node);
		FVertex* Merged = MakeVertex(Tail->InEdge, Head->OutEdge, Point, Time, Node);
		Kill(Tail, Node);
		Kill(Head, Node);
		Merged->OutInstance = Head->OutInstance;
		if (Tail->Previous == Head)
		{
			return;
		}
		Merged->Previous = Tail->Previous;
		Merged->Next = Head->Next;
		Tail->Previous->Next = Merged;
		Head->Next->Previous = Merged;
		Alive.push_back(Merged);
	}

	/** A reflex vertex runs into an edge and splits the wavefront in two. */
	void SplitEvent(std::vector<FVertex*>& Alive, FVertex* Vertex, FVertex* Tail, double Time)
	{
		FVertex* Head = Tail->Next;
		const FVector Point = Vertex->Position(Time);
		const int Node = AddNode(Point.X, Point.Y, Time);
		const FVector TailPoint = Tail->Position(Time);
		const FVector HeadPoint = Head->Position(Time);
		const int TailNode = AddNode(TailPoint.X, TailPoint.Y, Time);
		const int HeadNode = AddNode(HeadPoint.X, HeadPoint.Y, Time);
		FInstance* HitInstance = Tail->OutInstance;
		HitInstance->TailChain.push_back(TailNode);
		HitInstance->HeadChain.push_back(HeadNode);
		// Whatever the edge swept so far, closed along the wavefront through the hit point.
		std::vector<int> Polygon = {HitInstance->TailChain.front()};
		Polygon.insert(Polygon.end(), HitInstance->HeadChain.begin(), HitInstance->HeadChain.end());
		Polygon.push_back(Node);
		Polygon.insert(Polygon.end(), HitInstance->TailChain.rbegin(), HitInstance->TailChain.rend() - 1);
		AddFragment(HitInstance->Edge, Polygon);
		Vertex->Previous->OutInstance->HeadChain.push_back(Node);
		Vertex->OutInstance->TailChain.push_back(Node);
		FInstance* FirstPart = NewInstance(HitInstance->Edge, TailNode, Node);
		FInstance* SecondPart = NewInstance(HitInstance->Edge, Node, HeadNode);
		FVertex* AfterTail = MakeVertex(HitInstance->Edge, Vertex->OutEdge, Point, Time, Node);
		FVertex* BeforeHead = MakeVertex(Vertex->InEdge, HitInstance->Edge, Point, Time, Node);
		AfterTail->OutInstance = Vertex->OutInstance;
		BeforeHead->OutInstance = SecondPart;
		Tail->OutInstance = FirstPart;
		FVertex* Previous = Vertex->Previous;
		FVertex* Following = Vertex->Next;
		Tail->Next = AfterTail;
		AfterTail->Previous = Tail;
		AfterTail->Next = Following;
		Following->Previous = AfterTail;
		Previous->Next = BeforeHead;
		BeforeHead->Previous = Previous;
		BeforeHead->Next = Head;
		Head->Previous = BeforeHead;
		Kill(Vertex, Node);
		Alive.push_back(AfterTail);
		Alive.push_back(BeforeHead);
	}

	/** A loop of two vertices has collapsed onto a line: close its two faces; a loop of one is nothing. */
	void FinishSmallLoops(const std::vector<FVertex*>& Alive, double Time)
	{
		std::vector<const FVertex*> Seen;
		for (FVertex* Vertex : Alive)
		{
			if (!Vertex->bAlive || std::find(Seen.begin(), Seen.end(), Vertex) != Seen.end())
			{
				continue;
			}
			const std::vector<FVertex*> Loop = LoopVertices(Vertex);
			Seen.insert(Seen.end(), Loop.begin(), Loop.end());
			if (Loop.size() > 2)
			{
				continue;
			}
			if (Loop.size() == 1)
			{
				const FVector Position = Vertex->Position(Time);
				Kill(Vertex, AddNode(Position.X, Position.Y, Time));
				continue;
			}
			CloseTwoVertexLoop(Loop[0], Loop[1], Time);
		}
	}

	/** Closes the two faces of a loop that has collapsed onto a line. */
	void CloseTwoVertexLoop(FVertex* First, FVertex* Second, double Time)
	{
		int Nodes[2];
		FVertex* Members[2] = {First, Second};
		for (int Index = 0; Index < 2; ++Index)
		{
			const FVector Position = Members[Index]->Position(Time);
			Nodes[Index] = AddNode(Position.X, Position.Y, Time);
		}
		for (int Index = 0; Index < 2; ++Index)
		{
			FInstance* Instance = Members[Index]->OutInstance;
			Instance->TailChain.push_back(Nodes[Index]);
			Instance->HeadChain.push_back(Nodes[1 - Index]);
			AddFragment(Instance->Edge, ChainPolygon(*Instance));
		}
		for (int Index = 0; Index < 2; ++Index)
		{
			Kill(Members[Index], Nodes[Index]);
		}
	}

	/** Drops the vertices that are no longer alive. */
	static void RemoveDead(std::vector<FVertex*>& Alive)
	{
		std::vector<FVertex*> Kept;
		for (FVertex* Vertex : Alive)
		{
			if (Vertex->bAlive)
			{
				Kept.push_back(Vertex);
			}
		}
		Alive = std::move(Kept);
	}

	/** Processes events in time order until the wavefront is gone. */
	void RunEvents(const std::vector<FVertex*>& Vertices)
	{
		std::vector<FVertex*> Alive = Vertices;
		double Now = 0.0;
		size_t Guard = 0;
		while (!Alive.empty() && Guard < 4 * Vertices.size() + 20)
		{
			++Guard;
			const std::optional<FEvent> Event = NextEvent(Alive, Now);
			if (!Event.has_value())
			{
				break;
			}
			Now = std::max(Now, Event->Time);
			if (Event->Kind == EEventKind::Edge)
			{
				EdgeEvent(Alive, Event->First, Event->Second, Now);
			}
			else
			{
				SplitEvent(Alive, Event->First, Event->Second, Now);
			}
			RemoveDead(Alive);
			FinishSmallLoops(Alive, Now);
			RemoveDead(Alive);
		}
		if (!Alive.empty())
		{
			throw std::runtime_error("skeleton did not terminate");
		}
	}
};
}

FSkeleton StraightSkeleton(const std::vector<FRing>& Rings)
{
	FSkeleton Skeleton;
	FSkeletonBuilder Builder(Skeleton);
	Builder.Run(Rings);
	return Skeleton;
}
}
