// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "UnrealTournament.h"
#include "UnrealNetwork.h"

AUTInventory::AUTInventory(const FPostConstructInitializeProperties& PCIP)
: Super(PCIP)
{
	SetReplicates(true);
	bOnlyRelevantToOwner = true;
	bReplicateInstigator = true;

	RootComponent = PCIP.CreateDefaultSubobject<USceneComponent, USceneComponent>(this, TEXT("DummyRoot"), false, false, false);
	PickupMesh = PCIP.CreateDefaultSubobject<USkeletalMeshComponent, USkeletalMeshComponent>(this, TEXT("PickupMesh"), false, false, false);
	if (PickupMesh != NULL)
	{
		PickupMesh->AttachParent = RootComponent;
		PickupMesh->bAutoRegister = false;
	}
}

USkeletalMeshComponent* AUTInventory::GetPickupMeshTemplate_Implementation()
{
	return PickupMesh;
}

void AUTInventory::Destroyed()
{
	AUTCharacter* C = Cast<AUTCharacter>(Instigator);
	if (C != NULL)
	{
		C->RemoveInventory(this);
	}

	Super::Destroyed();
}

void AUTInventory::GivenTo(AUTCharacter* NewOwner, bool bAutoActivate)
{
	Instigator = NewOwner;
	SetOwner(NewOwner);
	UTOwner = NewOwner;
	eventGivenTo(NewOwner, bAutoActivate);
	ClientGivenTo(bAutoActivate);
}

void AUTInventory::Removed()
{
	ClientRemoved(); // must be first, since it won't replicate after Owner is lost

	Instigator = NULL;
	SetOwner(NULL);
	UTOwner = NULL;
	eventRemoved();
}

void AUTInventory::OnRep_Instigator()
{
	Super::OnRep_Instigator();
	if (bPendingClientGivenTo && Instigator != NULL)
	{
		bPendingClientGivenTo = false;
		ClientGivenTo_Implementation(bPendingAutoActivate);
	}
}

void AUTInventory::ClientGivenTo_Implementation(bool bAutoActivate)
{
	if (Instigator == NULL)
	{
		bPendingClientGivenTo = true;
		bPendingAutoActivate = bAutoActivate;
	}
	else
	{
		ClientGivenTo_Internal(bAutoActivate);
		eventClientGivenTo(bAutoActivate);
	}
}

void AUTInventory::ClientGivenTo_Internal(bool bAutoActivate)
{
	checkSlow(Instigator != NULL);
	SetOwner(Instigator);
	UTOwner = Cast<AUTCharacter>(Instigator);
	checkSlow(UTOwner != NULL);
}

void AUTInventory::ClientRemoved_Implementation()
{
	eventClientRemoved();
}

void AUTInventory::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(AUTInventory, NextInventory, COND_OwnerOnly);
}

void AUTInventory::DropFrom(const FVector& StartLocation, const FVector& TossVelocity)
{
	if (UTOwner != NULL)
	{
		UTOwner->RemoveInventory(this);
	}
	Destroy();
}