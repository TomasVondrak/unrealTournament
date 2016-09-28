// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "UnrealTournament.h"
#include "UnrealNetwork.h"
#include "UTWeap_GrenadeLauncher.h"

AUTWeap_GrenadeLauncher::AUTWeap_GrenadeLauncher()
{
	DefaultGroup = 3;
}

bool AUTWeap_GrenadeLauncher::BeginFiringSequence(uint8 FireModeNum, bool bClientFired)
{
	if (bHasStickyGrenades && FireModeNum == 1)
	{
		if (Role == ROLE_Authority)
		{
			DetonateStickyGrenades();
		}

		return true;
	}

	return Super::BeginFiringSequence(FireModeNum, bClientFired);
}

void AUTWeap_GrenadeLauncher::RegisterStickyGrenade(AUTProj_Grenade_Sticky* InGrenade)
{
	ActiveGrenades.AddUnique(InGrenade);
	bHasStickyGrenades = true;
	ActiveStickyGrenadeCount = ActiveGrenades.Num();
}

void AUTWeap_GrenadeLauncher::UnregisterStickyGrenade(AUTProj_Grenade_Sticky* InGrenade)
{
	ActiveGrenades.Remove(InGrenade);
	if (ActiveGrenades.Num() == 0)
	{
		bHasStickyGrenades = false;
	}
	ActiveStickyGrenadeCount = ActiveGrenades.Num();
}

void AUTWeap_GrenadeLauncher::DetonateStickyGrenades()
{
	for (int i = 0; i < ActiveGrenades.Num(); i++)
	{
		if (ActiveGrenades[i] != nullptr)
		{
			ActiveGrenades[i]->Explode(ActiveGrenades[i]->GetActorLocation(), FVector(0, 0, 1), nullptr);
		}
	}
	ActiveGrenades.Empty();
	bHasStickyGrenades = false;
	ActiveStickyGrenadeCount = ActiveGrenades.Num();
}

void AUTWeap_GrenadeLauncher::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AUTWeap_GrenadeLauncher, bHasStickyGrenades);
	DOREPLIFETIME(AUTWeap_GrenadeLauncher, ActiveStickyGrenadeCount);
}
