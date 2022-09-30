// Fill out your copyright notice in the Description page of Project Settings.


#include "InvalidTargetFinderProcessor.h"
#include "MassEnemyTargetFinderProcessor.h"
#include "MassTrackTargetProcessor.h"
#include "MassCommonFragments.h"
#include "MassMoveToCommandProcessor.h"
#include "MassSignalSubsystem.h"
#include "MassStateTreeTypes.h"
#include "MassNavigationSubsystem.h"
#include "MassEntityView.h"
#include "MassProjectileDamageProcessor.h"

void CopyMoveTarget(const FMassMoveTargetFragment& Source, FMassMoveTargetFragment& Destination, const UWorld& World)
{
	Destination.CreateNewAction(Source.GetCurrentAction(), World);
	Destination.bOffBoundaries = Source.bOffBoundaries;
	Destination.Center = Source.Center;
	Destination.Forward = Source.Forward;
	Destination.DistanceToGoal = Source.DistanceToGoal;
	Destination.DesiredSpeed = Source.DesiredSpeed;
	Destination.SlackRadius = Source.SlackRadius;
	Destination.bOffBoundaries = Source.bOffBoundaries;
	Destination.bSteeringFallingBehind = Source.bSteeringFallingBehind;
	Destination.IntentAtGoal = Source.IntentAtGoal;
}

UInvalidTargetFinderProcessor::UInvalidTargetFinderProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ExecutionFlags = (int32)EProcessorExecutionFlags::All;
}

void UInvalidTargetFinderProcessor::Initialize(UObject& Owner)
{
	SignalSubsystem = UWorld::GetSubsystem<UMassSignalSubsystem>(Owner.GetWorld());
	NavigationSubsystem = UWorld::GetSubsystem<UMassNavigationSubsystem>(Owner.GetWorld());
}

void UInvalidTargetFinderProcessor::ConfigureQueries()
{
	EntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FTargetEntityFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddRequirement<FTeamMemberFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassStashedMoveTargetFragment>(EMassFragmentAccess::ReadOnly, EMassFragmentPresence::Optional);
	EntityQuery.AddRequirement<FMassMoveTargetFragment>(EMassFragmentAccess::ReadWrite, EMassFragmentPresence::Optional);
	EntityQuery.AddTagRequirement<FMassWillNeedEnemyTargetTag>(EMassFragmentPresence::All);
}

bool IsTargetEntityOutOfRange(const FVector& TargetEntityLocation, const FVector &EntityLocation, const UMassEntitySubsystem& EntitySubsystem, FTargetEntityFragment& TargetEntityFragment, const FMassEntityHandle Entity, const FMassExecutionContext& Context)
{
	const double& DistanceBetweenEntities = (TargetEntityLocation - EntityLocation).Size();

	const bool& bIsEntitySoldier = Context.DoesArchetypeHaveTag<FMassProjectileDamagableSoldierTag>();
	const float MaxRange = GetEntityRange(bIsEntitySoldier);

	if (UE::Mass::Debug::IsDebuggingEntity(Entity))
	{
		AsyncTask(ENamedThreads::GameThread, [World = EntitySubsystem.GetWorld(), EntityLocation, TargetEntityLocation]()
		{
			DrawDebugDirectionalArrow(World, EntityLocation, TargetEntityLocation, 10.f, FColor::Yellow, false, 0.1f);
		});
	}

	return DistanceBetweenEntities > MaxRange;
}

bool DidCapsulesCollide(const FCapsule& Capsule1, const FCapsule& Capsule2, const FMassEntityHandle& Entity, const UWorld& World)
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("DidCapsulesCollide");

	const bool Result = TestCapsuleCapsule(Capsule1, Capsule2);

	if (UE::Mass::Debug::IsDebuggingEntity(Entity))
	{
		AsyncTask(ENamedThreads::GameThread, [Capsule1, Capsule2, &World, Result]()
		{
			const FLinearColor& Color = Result ? FLinearColor::Red : FLinearColor::Green;
			DrawCapsule(Capsule1, World, Color, false, 0.1f);
			DrawCapsule(Capsule2, World, Color, false, 0.1f);
		});
	}

	return Result;
}

bool IsTargetEntityObstructed(const FVector& EntityLocation, const FVector& TargetEntityLocation, const FNavigationObstacleHashGrid2D& AvoidanceObstacleGrid, const FMassEntityHandle& Entity, const UMassEntitySubsystem& EntitySubsystem, const bool& IsEntityOnTeam1, const FMassExecutionContext& Context, FTargetEntityFragment& TargetEntityFragment, const FMassEntityView& TargetEntityView, const FTransform& EntityTransform)
{
	FVector Buffer(10.f, 10.f, 10.f); // We keep a buffer in case EntityLocation and TargetEntityLocation are same value on any axis.
	FBox QueryBounds(EntityLocation.ComponentMin(TargetEntityLocation) - Buffer, EntityLocation.ComponentMax(TargetEntityLocation) + Buffer);
	TArray<FMassNavigationObstacleItem> CloseEntities;
	AvoidanceObstacleGrid.Query(QueryBounds, CloseEntities);

	if (UE::Mass::Debug::IsDebuggingEntity(Entity))
	{
		AsyncTask(ENamedThreads::GameThread, [QueryBounds, World = EntitySubsystem.GetWorld()]()
		{
			const FVector QueryCenter = (QueryBounds.Min + QueryBounds.Max) / 2.f;
			const FVector VerticalOffset(0.f, 0.f, 1000.f);
			DrawDebugBox(World, QueryCenter, QueryBounds.Max - QueryCenter + VerticalOffset, FColor::Blue, false, 0.1f);
		});
	}

	const bool& bIsEntitySoldier = Context.DoesArchetypeHaveTag<FMassProjectileDamagableSoldierTag>();
	const bool& bIsTargetEntitySoldier = TargetEntityView.HasTag<FMassProjectileDamagableSoldierTag>();
	const FCapsule& ProjectileTraceCapsule = GetProjectileTraceCapsuleToTarget(bIsEntitySoldier, bIsTargetEntitySoldier, EntityTransform, TargetEntityLocation);

	for (const FMassNavigationObstacleItem& OtherEntity : CloseEntities)
	{
		// Skip self.
		if (OtherEntity.Entity == Entity)
		{
			continue;
		}

		// Skip invalid entities.
		if (!EntitySubsystem.IsEntityValid(OtherEntity.Entity))
		{
			continue;
		}

		FMassEntityView OtherEntityView(EntitySubsystem, OtherEntity.Entity);
		FTeamMemberFragment* OtherEntityTeamMemberFragment = OtherEntityView.GetFragmentDataPtr<FTeamMemberFragment>();

		// Skip entities that don't have FTeamMemberFragment.
		if (!OtherEntityTeamMemberFragment) {
			continue;
		}

		// If same team or undamagable, check for collision.
		if (IsEntityOnTeam1 == OtherEntityTeamMemberFragment->IsOnTeam1 || !CanEntityDamageTargetEntity(TargetEntityFragment, OtherEntityView)) {
			FCapsule OtherEntityCapsule = MakeCapsuleForEntity(OtherEntityView);
			if (DidCapsulesCollide(ProjectileTraceCapsule, OtherEntityCapsule, Entity, *EntitySubsystem.GetWorld()))
			{
				return true;
			}
		}
	}

	return false;
}

bool IsTargetValid(const FMassEntityHandle& Entity, FMassEntityHandle& TargetEntity, const UMassEntitySubsystem& EntitySubsystem, const FVector& EntityLocation, FTargetEntityFragment& TargetEntityFragment, const FNavigationObstacleHashGrid2D& AvoidanceObstacleGrid, const bool& IsEntityOnTeam1, const FMassExecutionContext& Context, const FTransform& EntityTransform)
{
	if (!EntitySubsystem.IsEntityValid(TargetEntity))
	{
		return false;
	}

  const FMassEntityView TargetEntityView(EntitySubsystem, TargetEntity);
	const FVector& TargetEntityLocation = TargetEntityView.GetFragmentData<FTransformFragment>().GetTransform().GetLocation();
	if (IsTargetEntityOutOfRange(TargetEntityLocation, EntityLocation, EntitySubsystem, TargetEntityFragment, Entity, Context))
	{
		return false;
	}

	if (IsTargetEntityObstructed(EntityLocation, TargetEntityLocation, AvoidanceObstacleGrid, Entity, EntitySubsystem, IsEntityOnTeam1, Context, TargetEntityFragment, TargetEntityView, EntityTransform))
	{
		return false;
	}

	return true;
}

void ProcessEntity(const FMassExecutionContext& Context, const FMassEntityHandle Entity, const UMassEntitySubsystem& EntitySubsystem, FTargetEntityFragment& TargetEntityFragment, const FVector &EntityLocation, const FMassStashedMoveTargetFragment* StashedMoveTargetFragment, FMassMoveTargetFragment* MoveTargetFragment, TQueue<FMassEntityHandle>& EntitiesWithInvalidTargetQueue, const FNavigationObstacleHashGrid2D& AvoidanceObstacleGrid, const bool& IsEntityOnTeam1, const FTransform& EntityTransform, TQueue<FMassEntityHandle>& EntitiesWithUnstashedMovedTargetQueue)
{
	FMassEntityHandle& TargetEntity = TargetEntityFragment.Entity;
	if (!IsTargetValid(Entity, TargetEntity, EntitySubsystem, EntityLocation, TargetEntityFragment, AvoidanceObstacleGrid, IsEntityOnTeam1, Context, EntityTransform))
	{
		TargetEntity.Reset();
		EntitiesWithInvalidTargetQueue.Enqueue(Entity);

		// Unstash move target if needed.
		if (Context.DoesArchetypeHaveTag<FMassHasStashedMoveTargetTag>() && StashedMoveTargetFragment && MoveTargetFragment)
		{
			CopyMoveTarget(*StashedMoveTargetFragment, *MoveTargetFragment, *EntitySubsystem.GetWorld());
			EntitiesWithUnstashedMovedTargetQueue.Enqueue(Entity);
		}
	}
}

void UInvalidTargetFinderProcessor::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("UInvalidTargetFinderProcessor");

	if (!NavigationSubsystem)
	{
		return;
	}

	TQueue<FMassEntityHandle> EntitiesWithInvalidTargetQueue;
	TQueue<FMassEntityHandle> EntitiesWithUnstashedMovedTargetQueue;

  {
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("UInvalidTargetFinderProcessor.Execute.ForEachEntityChunk");

		// This is not ParallelForEachEntityChunk because there are few chunks. Instead we ParallelFor within the chunk.
	  EntityQuery.ForEachEntityChunk(EntitySubsystem, Context, [&EntitySubsystem, &NavigationSubsystem = NavigationSubsystem, &EntitiesWithInvalidTargetQueue, &EntitiesWithUnstashedMovedTargetQueue](FMassExecutionContext& Context)
    {
      const int32 NumEntities = Context.GetNumEntities();

      const TConstArrayView<FTransformFragment> TransformList = Context.GetFragmentView<FTransformFragment>();
      const TArrayView<FTargetEntityFragment> TargetEntityList = Context.GetMutableFragmentView<FTargetEntityFragment>();
      const TConstArrayView<FMassStashedMoveTargetFragment> StashedMoveTargetList = Context.GetFragmentView<FMassStashedMoveTargetFragment>();
      const TArrayView<FMassMoveTargetFragment> MoveTargetList = Context.GetMutableFragmentView<FMassMoveTargetFragment>();
      const TConstArrayView<FTeamMemberFragment> TeamMemberList = Context.GetFragmentView<FTeamMemberFragment>();

      // TODO: We're incorrectly assuming all obstacles can be targets.
      const FNavigationObstacleHashGrid2D& AvoidanceObstacleGrid = NavigationSubsystem->GetObstacleGrid();

			ParallelFor(NumEntities, [&](const int32 EntityIndex) {
				ProcessEntity(Context, Context.GetEntity(EntityIndex), EntitySubsystem, TargetEntityList[EntityIndex], TransformList[EntityIndex].GetTransform().GetLocation(), StashedMoveTargetList.Num() > 0 ? &StashedMoveTargetList[EntityIndex] : nullptr, MoveTargetList.Num() > 0 ? &MoveTargetList[EntityIndex] : nullptr, EntitiesWithInvalidTargetQueue, AvoidanceObstacleGrid, TeamMemberList[EntityIndex].IsOnTeam1, TransformList[EntityIndex].GetTransform(), EntitiesWithUnstashedMovedTargetQueue);
			});
    });
  }


	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("UInvalidTargetFinderProcessor.Execute.ProcessQueues");

		TransientEntitiesToSignal.Reset();

		while (!EntitiesWithInvalidTargetQueue.IsEmpty())
		{
			FMassEntityHandle Entity;
			const bool bSuccess = EntitiesWithInvalidTargetQueue.Dequeue(Entity);
			check(bSuccess);

			Context.Defer().AddTag<FMassNeedsEnemyTargetTag>(Entity);
			Context.Defer().RemoveTag<FMassWillNeedEnemyTargetTag>(Entity);
			Context.Defer().RemoveTag<FMassTrackTargetTag>(Entity);
			TransientEntitiesToSignal.Add(Entity);
		}

	  while (!EntitiesWithUnstashedMovedTargetQueue.IsEmpty())
		{
			FMassEntityHandle Entity;
			const bool bSuccess = EntitiesWithUnstashedMovedTargetQueue.Dequeue(Entity);
			check(bSuccess);

			Context.Defer().RemoveTag<FMassHasStashedMoveTargetTag>(Entity);
		}

		if (TransientEntitiesToSignal.Num())
		{
			SignalSubsystem->SignalEntities(UE::Mass::Signals::NewStateTreeTaskRequired, TransientEntitiesToSignal);
		}
	}
}
