// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "UnrealTournament.h"
#include "UTCharacterMovement.h"
#include "ActiveSound.h"
#include "AudioDevice.h"
#include "UTPickupInventory.h"
#include "UTPickupWeapon.h"
#include "UTAnnouncer.h"
#include "UTHUDWidgetMessage.h"
#include "UTPlayerInput.h"
#include "UTPlayerCameraManager.h"
#include "UTCheatManager.h"
#include "UTCTFGameState.h"
#include "UTChatMessage.h"
#include "Engine/Console.h"
#include "UTAnalytics.h"
#include "Runtime/Analytics/Analytics/Public/Analytics.h"
#include "Runtime/Analytics/Analytics/Public/Interfaces/IAnalyticsProvider.h"
#include "Online.h"
#include "UTOnlineGameSearchBase.h"
#include "OnlineSubsystemTypes.h"
#include "UTDroppedPickup.h"
#include "UTGameEngine.h"
#include "UnrealNetwork.h"
#include "UTProfileSettings.h"
#include "UTViewPlaceholder.h"

DEFINE_LOG_CATEGORY_STATIC(LogUTPlayerController, Log, All);

AUTPlayerController::AUTPlayerController(const class FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// set our turn rates for input
	BaseTurnRate = 45.f;
	BaseLookUpRate = 45.f;

	bAutoWeaponSwitch = true;

	MaxDodgeClickTime = 0.25f;
	MaxDodgeTapTime = 0.3f;
	LastTapLeftTime = -10.f;
	LastTapRightTime = -10.f;
	LastTapForwardTime = -10.f;
	LastTapBackTime = -10.f;
	bSingleTapWallDodge = true;
	bSingleTapAfterJump = true;
	bTapCrouchToSlide = true;
	CrouchRollTapInterval = 0.25f;

	PlayerCameraManagerClass = AUTPlayerCameraManager::StaticClass();
	CheatClass = UUTCheatManager::StaticClass();

	WeaponBobGlobalScaling = 1.f;
	EyeOffsetGlobalScaling = 1.f;

	ConfigDefaultFOV = 100.0f;
	FFAPlayerColor = FLinearColor(0.020845f, 0.335f, 0.0f, 1.0f);

	LastEmoteTime = 0.0f;
	EmoteCooldownTime = 0.3f;

	bAutoSlide = false;
	bHoldAccelWithSlideRoll = true;

	bSpectateBehindView = true;

	PredictionFudgeFactor = 30.f;
	MaxPredictionPing = 0.f; 
	DesiredPredictionPing = 0.f;

	bIsDebuggingProjectiles = false;
}

void AUTPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (Role < ROLE_Authority)
	{
		ServerNegotiatePredictionPing(DesiredPredictionPing);
	}
}

void AUTPlayerController::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION(AUTPlayerController, MaxPredictionPing, COND_OwnerOnly);
}

void AUTPlayerController::ServerNegotiatePredictionPing_Implementation(float NewPredictionPing)
{
	MaxPredictionPing = FMath::Clamp(NewPredictionPing, 0.f, UUTGameEngine::StaticClass()->GetDefaultObject<UUTGameEngine>()->ServerMaxPredictionPing);
}

bool AUTPlayerController::ServerNegotiatePredictionPing_Validate(float NewPredictionPing)
{
	return true;
}

void AUTPlayerController::Predict(float NewPredictionPing)
{
	DesiredPredictionPing = NewPredictionPing;
	SaveConfig();
	ServerNegotiatePredictionPing(NewPredictionPing);
}

float AUTPlayerController::GetPredictionTime()
{
	// exact ping is in msec, divide by 1000 to get time in seconds
	//if (Role == ROLE_Authority) { UE_LOG(UT, Warning, TEXT("Server ExactPing %f"), PlayerState->ExactPing); }
	return (PlayerState && (GetNetMode() != NM_Standalone)) ? (0.0005f*FMath::Clamp(PlayerState->ExactPing - PredictionFudgeFactor, 0.f, MaxPredictionPing)) : 0.f;
}

float AUTPlayerController::GetProjectileSleepTime()
{
	return 0.001f * FMath::Max(0.f, PlayerState->ExactPing - PredictionFudgeFactor - MaxPredictionPing);
}

void AUTPlayerController::NP()
{
	ServerNP();
}

bool AUTPlayerController::ServerNP_Validate()
{
	return true;
}

void AUTPlayerController::ServerNP_Implementation()
{
	if (Player && UUTGameEngine::StaticClass()->GetDefaultObject<UUTGameEngine>()->bAllowClientNetProfile)
	{
		Player->Exec(GetWorld(), *FString::Printf(TEXT("NETPROFILE")), *GLog);
	}
}

bool AUTPlayerController::ServerNotifyProjectileHit_Validate(AUTProjectile* HitProj, FVector HitLocation, AActor* DamageCauser, float TimeStamp)
{
	return true;
}

void AUTPlayerController::ServerNotifyProjectileHit_Implementation(AUTProjectile* HitProj, FVector HitLocation, AActor* DamageCauser, float TimeStamp)
{
	// @TODO FIXMESTEVE - need to verify shot from player's location at timestamp to HitLocation is valid, and that projectile should have been there at that time
	if (HitProj)
	{
		HitProj->NotifyClientSideHit(this, HitLocation, DamageCauser);
	}
}

void AUTPlayerController::ToggleSingleTap()
{
	bSingleTapWallDodge = !bSingleTapWallDodge;
}

void AUTPlayerController::ToggleHoldAccel()
{
	bHoldAccelWithSlideRoll = !bHoldAccelWithSlideRoll;
	UUTCharacterMovement* MyCharMovement = UTCharacter ? UTCharacter->UTCharacterMovement : NULL;
	if (MyCharMovement)
	{
		MyCharMovement->bMaintainSlideRollAccel = bHoldAccelWithSlideRoll;
	}
}

void AUTPlayerController::ToggleAutoSlide()
{
	SetAutoSlide(!bAutoSlide);
}

void AUTPlayerController::SetAutoSlide(bool bNewAutoSlide)
{
	bAutoSlide = bNewAutoSlide;
	UUTCharacterMovement* MyCharMovement = UTCharacter ? UTCharacter->UTCharacterMovement : NULL;
	if (MyCharMovement)
	{
		MyCharMovement->bAutoSlide = bAutoSlide;
	}
	if (Role != ROLE_Authority)
	{
		// @TODO FIXMESTEVE - only replicate if we know it has changed
		ServerSetAutoSlide(bAutoSlide);
	}
}

void AUTPlayerController::ServerSetAutoSlide_Implementation(bool bNewAutoSlide)
{
	SetAutoSlide(bNewAutoSlide);
}

bool AUTPlayerController::ServerSetAutoSlide_Validate(bool bNewAutoSlide)
{
	return true;
}

void AUTPlayerController::SetEyeOffsetScaling(float NewScaling)
{
	EyeOffsetGlobalScaling = NewScaling;
}

void AUTPlayerController::SetWeaponBobScaling(float NewScaling)
{
	WeaponBobGlobalScaling = NewScaling;
}

FVector AUTPlayerController::GetFocalLocation() const
{
	if (GetPawnOrSpectator())
	{
		return GetPawnOrSpectator()->GetPawnViewLocation();
	}
	else
	{
		return GetSpawnLocation();
	}
}

void AUTPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	InputComponent->BindAxis("MoveForward", this, &AUTPlayerController::MoveForward);
	InputComponent->BindAxis("MoveBackward", this, &AUTPlayerController::MoveBackward);
	InputComponent->BindAxis("MoveLeft", this, &AUTPlayerController::MoveLeft);
	InputComponent->BindAxis("MoveRight", this, &AUTPlayerController::MoveRight);
	InputComponent->BindAxis("MoveUp", this, &AUTPlayerController::MoveUp);
	InputComponent->BindAction("Jump", IE_Pressed, this, &AUTPlayerController::Jump);
	InputComponent->BindAction("Crouch", IE_Pressed, this, &AUTPlayerController::Crouch);
	InputComponent->BindAction("Crouch", IE_Released, this, &AUTPlayerController::UnCrouch);
	InputComponent->BindAction("ToggleCrouch", IE_Pressed, this, &AUTPlayerController::ToggleCrouch);

	InputComponent->BindAction("TapLeft", IE_Pressed, this, &AUTPlayerController::OnTapLeft);
	InputComponent->BindAction("TapRight", IE_Pressed, this, &AUTPlayerController::OnTapRight);
	InputComponent->BindAction("TapForward", IE_Pressed, this, &AUTPlayerController::OnTapForward);
	InputComponent->BindAction("TapBack", IE_Pressed, this, &AUTPlayerController::OnTapBack);
	InputComponent->BindAction("SingleTapDodge", IE_Pressed, this, &AUTPlayerController::OnSingleTapDodge);
	InputComponent->BindAction("HoldDodge", IE_Pressed, this, &AUTPlayerController::HoldDodge);
	InputComponent->BindAction("HoldDodge", IE_Released, this, &AUTPlayerController::ReleaseDodge);
	InputComponent->BindAction("HoldRollSlide", IE_Pressed, this, &AUTPlayerController::HoldRollSlide);
	InputComponent->BindAction("HoldRollSlide", IE_Released, this, &AUTPlayerController::ReleaseRollSlide);

	InputComponent->BindAction("TapLeftRelease", IE_Released, this, &AUTPlayerController::OnTapLeftRelease);
	InputComponent->BindAction("TapRightRelease", IE_Released, this, &AUTPlayerController::OnTapRightRelease);
	InputComponent->BindAction("TapForwardRelease", IE_Released, this, &AUTPlayerController::OnTapForwardRelease);
	InputComponent->BindAction("TapBackRelease", IE_Released, this, &AUTPlayerController::OnTapBackRelease);

	// We have 2 versions of the rotation bindings to handle different kinds of devices differently
	// "turn" handles devices that provide an absolute delta, such as a mouse.
	// "turnrate" is for devices that we choose to treat as a rate of change, such as an analog joystick
	InputComponent->BindAxis("Turn", this, &APlayerController::AddYawInput);
	InputComponent->BindAxis("TurnRate", this, &AUTPlayerController::TurnAtRate);
	InputComponent->BindAxis("LookUp", this, &APlayerController::AddPitchInput);
	InputComponent->BindAxis("LookUpRate", this, &AUTPlayerController::LookUpAtRate);

	InputComponent->BindAction("PrevWeapon", IE_Pressed, this, &AUTPlayerController::PrevWeapon);
	InputComponent->BindAction("NextWeapon", IE_Released, this, &AUTPlayerController::NextWeapon);
	InputComponent->BindAction("ThrowWeapon", IE_Released, this, &AUTPlayerController::ThrowWeapon);
	InputComponent->BindAction("ToggleTranslocator", IE_Pressed, this, &AUTPlayerController::ToggleTranslocator);

	InputComponent->BindAction("StartFire", IE_Pressed, this, &AUTPlayerController::OnFire);
	InputComponent->BindAction("StopFire", IE_Released, this, &AUTPlayerController::OnStopFire);
	InputComponent->BindAction("StartAltFire", IE_Pressed, this, &AUTPlayerController::OnAltFire);
	InputComponent->BindAction("StopAltFire", IE_Released, this, &AUTPlayerController::OnStopAltFire);
	InputComponent->BindTouch(EInputEvent::IE_Pressed, this, &AUTPlayerController::TouchStarted);

	InputComponent->BindAction("ShowScores", IE_Pressed, this, &AUTPlayerController::OnShowScores);
	InputComponent->BindAction("ShowScores", IE_Released, this, &AUTPlayerController::OnHideScores);

	InputComponent->BindAction("Talk", IE_Pressed, this, &AUTPlayerController::Talk);
	InputComponent->BindAction("TeamTalk", IE_Pressed, this, &AUTPlayerController::TeamTalk);

	InputComponent->BindAction("FasterEmote", IE_Pressed, this, &AUTPlayerController::FasterEmote);
	InputComponent->BindAction("SlowerEmote", IE_Pressed, this, &AUTPlayerController::SlowerEmote);
	InputComponent->BindAction("PlayEmote1", IE_Pressed, this, &AUTPlayerController::PlayEmote1);
	InputComponent->BindAction("PlayEmote2", IE_Pressed, this, &AUTPlayerController::PlayEmote2);
	InputComponent->BindAction("PlayEmote3", IE_Pressed, this, &AUTPlayerController::PlayEmote3);
}

void AUTPlayerController::ProcessPlayerInput(const float DeltaTime, const bool bGamePaused)
{
	Super::ProcessPlayerInput(DeltaTime, bGamePaused);

	if (bRequestedDodge)
	{
		PerformSingleTapDodge();
	}
	bRequestedDodge = false;
	MovementForwardAxis = 0.f;
	MovementStrafeAxis = 0.f;
}

void AUTPlayerController::InitInputSystem()
{
	if (PlayerInput == NULL)
	{
		PlayerInput = ConstructObject<UUTPlayerInput>(UUTPlayerInput::StaticClass(), this);
	}

	Super::InitInputSystem();

	if (RewardAnnouncerPath.AssetLongPathname.Len() > 0)
	{
		TSubclassOf<UUTAnnouncer> RewardAnnouncerClass = LoadClass<UUTAnnouncer>(NULL, *RewardAnnouncerPath.AssetLongPathname, NULL, 0, NULL);
		if (RewardAnnouncerClass != NULL && RewardAnnouncerClass.GetDefaultObject()->IsRewardAnnouncer())
		{
			RewardAnnouncer = NewObject<UUTAnnouncer>(this, RewardAnnouncerClass);
		}
	}
	if (StatusAnnouncerPath.AssetLongPathname.Len() > 0)
	{
		TSubclassOf<UUTAnnouncer> StatusAnnouncerClass = LoadClass<UUTAnnouncer>(NULL, *StatusAnnouncerPath.AssetLongPathname, NULL, 0, NULL);
		if (StatusAnnouncerClass != NULL && StatusAnnouncerClass.GetDefaultObject()->IsStatusAnnouncer())
		{
			StatusAnnouncer = NewObject<UUTAnnouncer>(this, StatusAnnouncerClass);
		}
	}
}

// @TODO FIXMESTEVE temporary until we have version checking working correctly
void AUTPlayerController::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	if ((GetNetMode() == NM_Client) && NetConnection && (NetConnection->NegotiatedVer != GEngineNetVersion))
	{
		// this should only happen if server is pre-official client
		UE_LOG(UT, Error, TEXT("Server is outdated:  local version %d negotiated from server %d"), GEngineNetVersion, NetConnection->NegotiatedVer);
	}
}

/* Cache a copy of the PlayerState cast'd to AUTPlayerState for easy reference.  Do it both here and when the replicated copy of APlayerState arrives in OnRep_PlayerState */
void AUTPlayerController::InitPlayerState()
{
	Super::InitPlayerState();
	
	// need this until Controller::InitPlayerState() is updated
	if (PlayerState && PlayerState->PlayerName.IsEmpty())
	{
		UWorld* const World = GetWorld();
		if (World)
		{
			AGameMode* const GameMode = World->GetAuthGameMode();
			if (GameMode)
			{
				// don't call SetPlayerName() as that will broadcast entry messages but the GameMode hasn't had a chance
				// to potentially apply a player/bot name yet
				PlayerState->PlayerName = GameMode->DefaultPlayerName;
			}
		}
	}
}

void AUTPlayerController::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	UTPlayerState = Cast<AUTPlayerState>(PlayerState);
}

void AUTPlayerController::SetPawn(APawn* InPawn)
{
	if (InPawn == NULL)
	{
		// Attempt to move the PC to the current camera location if no pawn was specified
		const FVector NewLocation = (PlayerCameraManager != NULL) ? PlayerCameraManager->GetCameraLocation() : GetSpawnLocation();
		SetSpawnLocation(NewLocation);
	}

	AController::SetPawn(InPawn);

	UTCharacter = Cast<AUTCharacter>(InPawn);

	if (Player && IsLocalPlayerController())
	{
		// apply FOV angle if dead/spectating
		if (GetPawn() == NULL && PlayerCameraManager != NULL)
		{
			FOV(ConfigDefaultFOV);
		}
		if (UTCharacter && UTCharacter->UTCharacterMovement)
		{
			UTCharacter->UTCharacterMovement->UpdateSlideRoll(bIsHoldingSlideRoll);
			SetAutoSlide(bAutoSlide);
			UTCharacter->UTCharacterMovement->bMaintainSlideRollAccel = bHoldAccelWithSlideRoll;
		}
	}
}

void AUTPlayerController::SpawnPlayerCameraManager()
{
	Super::SpawnPlayerCameraManager();
	// init configured FOV angle
	if (PlayerCameraManager != NULL)
	{
		PlayerCameraManager->DefaultFOV = ConfigDefaultFOV;
	}
}

void AUTPlayerController::ClientRestart_Implementation(APawn* NewPawn)
{
	Super::ClientRestart_Implementation(NewPawn);

	// make sure we don't have leftover zoom
	if (PlayerCameraManager != NULL)
	{
		PlayerCameraManager->UnlockFOV();
		PlayerCameraManager->DefaultFOV = ConfigDefaultFOV;
	}

	SetCameraMode("Default");
}

void AUTPlayerController::PawnPendingDestroy(APawn* InPawn)
{
	if (IsInState(NAME_Inactive))
	{
		UE_LOG(LogPath, Log, TEXT("PawnPendingDestroy while inactive %s"), *GetName());
	}

	if (InPawn == GetPawn() && InPawn != NULL)
	{
		GetPawn()->UnPossessed();
		SetPawn(NULL);

		FRotator AdjustedCameraRot = GetControlRotation();
		AdjustedCameraRot.Pitch = -45.0f;
		AdjustedCameraRot.Roll = 0.f;
		SetControlRotation(AdjustedCameraRot);

		ChangeState(NAME_Inactive);

		if (PlayerState == NULL)
		{
			Destroy();
		}
	}
}

void AUTPlayerController::FOV(float NewFOV)
{
	if (NewFOV != ConfigDefaultFOV)
	{
		ConfigDefaultFOV = FMath::Clamp<float>(NewFOV, FOV_CONFIG_MIN, FOV_CONFIG_MAX);
		if (PlayerCameraManager != NULL)
		{
			PlayerCameraManager->DefaultFOV = ConfigDefaultFOV;
		}
		if (GetPawn() != NULL && GetNetMode() != NM_Standalone)
		{
			Suicide();
		}
		SaveConfig();
	}
}

bool AUTPlayerController::InputKey(FKey Key, EInputEvent EventType, float AmountDepressed, bool bGamepad)
{
	// unfortunately have to go roundabout because this is the only InputKey() that's virtual
	UUTPlayerInput* Input = Cast<UUTPlayerInput>(PlayerInput);
	if (Input != NULL)
	{
		Input->ExecuteCustomBind(Key, EventType);
		// ...unsure if we should eat the input on success
	}
	return Super::InputKey(Key, EventType, AmountDepressed, bGamepad);
}

void AUTPlayerController::SwitchToBestWeapon()
{
	if (UTCharacter != NULL && IsLocalPlayerController())
	{
		AUTWeapon* BestWeapon = NULL;
		float BestPriority = 0.0f;
		for (TInventoryIterator<AUTWeapon> It(UTCharacter); It; ++It)
		{
			if (It->HasAnyAmmo())
			{
				float TestPriority = It->GetAutoSwitchPriority();
				if (TestPriority > BestPriority)
				{
					BestWeapon = *It;
					BestPriority = TestPriority;
				}
			}
		}
		UTCharacter->SwitchWeapon(BestWeapon);
	}
}
void AUTPlayerController::PrevWeapon()
{
	SwitchWeaponInSequence(true);
}
void AUTPlayerController::NextWeapon()
{
	SwitchWeaponInSequence(false);
}
void AUTPlayerController::ToggleTranslocator()
{
	if (UTCharacter != NULL && UTCharacter->GetWeapon() != NULL && IsLocalPlayerController())
	{
		if (UTCharacter->GetWeapon()->Group == 0)
		{
			SwitchWeapon(PreviousWeaponGroup);
		}
		else
		{
			PreviousWeaponGroup = UTCharacter->GetWeapon()->Group;
			SwitchWeapon(0);
		}
	}
}

void AUTPlayerController::ThrowWeapon()
{
	if (UTCharacter != NULL && IsLocalPlayerController() && !UTCharacter->IsRagdoll())
	{
		if (UTCharacter->GetWeapon() != nullptr && UTCharacter->GetWeapon()->DroppedPickupClass != nullptr && UTCharacter->GetWeapon()->bCanThrowWeapon)
		{
			ServerThrowWeapon();
		}
	}
}

bool AUTPlayerController::ServerThrowWeapon_Validate()
{
	return true;
}

void AUTPlayerController::ServerThrowWeapon_Implementation()
{
	if (UTCharacter != NULL && !UTCharacter->IsRagdoll())
	{
		if (UTCharacter->GetWeapon() != nullptr && UTCharacter->GetWeapon()->DroppedPickupClass != nullptr && UTCharacter->GetWeapon()->bCanThrowWeapon)
		{
			UTCharacter->TossInventory(UTCharacter->GetWeapon(), FVector(400.0f, 0, 200.f));
		}
	}
}

void AUTPlayerController::SwitchWeaponInSequence(bool bPrev)
{
	if (UTCharacter != NULL && IsLocalPlayerController() && UTCharacter->EmoteCount == 0 && !UTCharacter->IsRagdoll())
	{
		if (UTCharacter->GetWeapon() == NULL)
		{
			SwitchToBestWeapon();
		}
		else
		{
			AUTWeapon* Best = NULL;
			AUTWeapon* WraparoundChoice = NULL;
			int32 CurrentGroup = (UTCharacter->GetPendingWeapon() != NULL) ? UTCharacter->GetPendingWeapon()->Group : UTCharacter->GetWeapon()->Group;
			int32 CurrentSlot = (UTCharacter->GetPendingWeapon() != NULL) ? UTCharacter->GetPendingWeapon()->GroupSlot : UTCharacter->GetWeapon()->GroupSlot;
			for (TInventoryIterator<AUTWeapon> It(UTCharacter); It; ++It)
			{
				AUTWeapon* Weap = *It;
				if (Weap != UTCharacter->GetWeapon() && Weap->HasAnyAmmo())
				{
					if (bPrev)
					{
						if ( (Weap->Group < CurrentGroup || (Weap->Group == CurrentGroup && Weap->GroupSlot < CurrentSlot)) &&
							(Best == NULL || Weap->Group > Best->Group || Weap->GroupSlot > Best->GroupSlot) )
						{
							Best = Weap;
						}
						if (WraparoundChoice == NULL || Weap->Group > WraparoundChoice->Group || (Weap->Group == WraparoundChoice->Group && Weap->GroupSlot > WraparoundChoice->GroupSlot))
						{
							WraparoundChoice = Weap;
						}
					}
					else
					{
						if ( (Weap->Group > CurrentGroup || (Weap->Group == CurrentGroup && Weap->GroupSlot > CurrentSlot)) &&
							(Best == NULL || Weap->Group < Best->Group || Weap->GroupSlot < Best->GroupSlot) )
						{
							Best = Weap;
						}
						if (WraparoundChoice == NULL || Weap->Group < WraparoundChoice->Group || (Weap->Group == WraparoundChoice->Group && Weap->GroupSlot < WraparoundChoice->GroupSlot))
						{
							WraparoundChoice = Weap;
						}
					}
				}
			}
			if (Best == NULL)
			{
				Best = WraparoundChoice;
			}
			UTCharacter->SwitchWeapon(Best);
		}
	}
}
void AUTPlayerController::CheckAutoWeaponSwitch(AUTWeapon* TestWeapon)
{
	if (UTCharacter != NULL && IsLocalPlayerController())
	{
		AUTWeapon* CurWeapon = UTCharacter->GetPendingWeapon();
		if (CurWeapon == NULL)
		{
			CurWeapon = UTCharacter->GetWeapon();
		}
		if (CurWeapon == NULL || (bAutoWeaponSwitch && !UTCharacter->IsPendingFire(CurWeapon->GetCurrentFireMode()) && TestWeapon->GetAutoSwitchPriority() > CurWeapon->GetAutoSwitchPriority()))
		{
			UTCharacter->SwitchWeapon(TestWeapon);
		}
	}
}
void AUTPlayerController::SwitchWeapon(int32 Group)
{
	if (UTCharacter != NULL && IsLocalPlayerController() && UTCharacter->EmoteCount == 0 && !UTCharacter->IsRagdoll())
	{
		// if current weapon isn't in the specified group, pick lowest GroupSlot in that group
		// if it is, then pick next highest slot, or wrap around to lowest if no higher slot
		AUTWeapon* CurrWeapon = (UTCharacter->GetPendingWeapon() != NULL) ? UTCharacter->GetPendingWeapon() : UTCharacter->GetWeapon();
		AUTWeapon* LowestSlotWeapon = NULL;
		AUTWeapon* NextSlotWeapon = NULL;
		for (TInventoryIterator<AUTWeapon> It(UTCharacter); It; ++It)
		{
			AUTWeapon* Weap = *It;
			if (Weap != UTCharacter->GetWeapon() && Weap->HasAnyAmmo())
			{
				if (Weap->Group == Group)
				{
					if (LowestSlotWeapon == NULL || LowestSlotWeapon->GroupSlot > Weap->GroupSlot)
					{
						LowestSlotWeapon = Weap;
					}
					if (CurrWeapon != NULL && CurrWeapon->Group == Group && Weap->GroupSlot > CurrWeapon->GroupSlot && (NextSlotWeapon == NULL || NextSlotWeapon->GroupSlot > Weap->GroupSlot))
					{
						NextSlotWeapon = Weap;
					}
				}
			}
		}
		if (NextSlotWeapon != NULL)
		{
			UTCharacter->SwitchWeapon(NextSlotWeapon);
		}
		else if (LowestSlotWeapon != NULL)
		{
			UTCharacter->SwitchWeapon(LowestSlotWeapon);
		}
	}
}

void AUTPlayerController::OnFire()
{
	if (GetPawn() != NULL)
	{
		new(DeferredFireInputs) FDeferredFireInput(0, true);
	}
	else if (IsInState(NAME_Spectating))
	{
		if ((PlayerState == nullptr || !PlayerState->bOnlySpectator) && 
			bPlayerIsWaiting)
		{
			ServerRestartPlayer();
		}
		else
		{
			ServerViewNextPlayer();
		}
	}
	else
	{
		ServerRestartPlayer();
	}

}
void AUTPlayerController::OnStopFire()
{
	if (GetPawn() != NULL)
	{
		new(DeferredFireInputs) FDeferredFireInput(0, false);
	}
}
void AUTPlayerController::OnAltFire()
{
	if (GetPawn() != NULL)
	{
		new(DeferredFireInputs) FDeferredFireInput(1, true);
	}
}
void AUTPlayerController::OnStopAltFire()
{
	if (GetPawn() != NULL)
	{
		new(DeferredFireInputs) FDeferredFireInput(1, false);
	}
	else if (IsInState(NAME_Spectating))
	{
		if ((PlayerState == nullptr || !PlayerState->bOnlySpectator) &&
			bPlayerIsWaiting)
		{
			ServerRestartPlayer();
		}
		else
		{
			ServerViewSelf();
		}
	}
	else
	{
		ServerRestartPlayer();
	}
}

void AUTPlayerController::MoveForward(float Value)
{
	if (Value != 0.0f && UTCharacter != NULL)
	{
		MovementForwardAxis = Value;
		UTCharacter->MoveForward(Value);
	}
	else if (GetSpectatorPawn() != NULL)
	{
		GetSpectatorPawn()->MoveForward(Value);
	}
}

void AUTPlayerController::MoveBackward(float Value)
{
	MoveForward(Value * -1);
}

void AUTPlayerController::MoveLeft(float Value)
{
	MoveRight(Value * -1);
}


void AUTPlayerController::MoveRight(float Value)
{
	if (Value != 0.0f && UTCharacter != NULL)
	{
		MovementStrafeAxis = Value;
		UTCharacter->MoveRight(Value);
	}
	else if (GetSpectatorPawn() != NULL)
	{
		GetSpectatorPawn()->MoveRight(Value);
	}
}

void AUTPlayerController::MoveUp(float Value)
{
	if (Value != 0.0f && UTCharacter != NULL)
	{
		UTCharacter->MoveUp(Value);
	}
	else if (GetSpectatorPawn() != NULL)
	{
		GetSpectatorPawn()->MoveUp_World(Value);
	}
}

void AUTPlayerController::TurnAtRate(float Rate)
{
	// calculate delta for this frame from the rate information
	AddYawInput(Rate * BaseTurnRate * GetWorld()->GetDeltaSeconds());
}

void AUTPlayerController::LookUpAtRate(float Rate)
{
	// calculate delta for this frame from the rate information
	AddPitchInput(Rate * BaseLookUpRate * GetWorld()->GetDeltaSeconds());
}

void AUTPlayerController::Jump()
{
	if (GetCharacter() != NULL)
	{
		GetCharacter()->bPressedJump = true;
	}
}

void AUTPlayerController::Crouch()
{
	if (GetCharacter() != NULL)
	{
		GetCharacter()->Crouch(false);
	}
	RollTapThresholdTime = GetWorld()->GetTimeSeconds() + CrouchRollTapInterval;
}

void AUTPlayerController::UnCrouch()
{
	if (GetCharacter() != NULL)
	{
		GetCharacter()->UnCrouch(false);
		if (bTapCrouchToSlide && (GetWorld()->GetTimeSeconds() < RollTapThresholdTime) && (UTCharacter != NULL))
		{
			// tap roll
			RollTapThresholdTime = 0.f;
			UTCharacter->bRollNextUncrouch = true;
		}
	}
}

void AUTPlayerController::ToggleCrouch()
{
	if (GetCharacter() != nullptr)
	{
		GetCharacter()->bIsCrouched ? UnCrouch() : Crouch();
	}
}

void AUTPlayerController::DisplayDebug(UCanvas* Canvas, const FDebugDisplayInfo& DebugDisplay, float& YL, float& YPos)
{
	UFont* RenderFont = GEngine->GetSmallFont();
	if (GetPawn() == NULL)
	{
		if (PlayerState == NULL)
		{
			Canvas->DrawText(RenderFont, TEXT("NO PlayerState"), 4.0f, YPos);
		}
		else
		{
			PlayerState->DisplayDebug(Canvas, DebugDisplay, YL, YPos);
		}
		YPos += YL;

		return;
	}

	Canvas->SetDrawColor(255, 0, 0);
	Canvas->DrawText(RenderFont, FString::Printf(TEXT("CONTROLLER %s Pawn %s"), *GetName(), *GetPawn()->GetName()), 4.0f, YPos);
	YPos += YL;

	Canvas->SetDrawColor(255, 255, 0);
	Canvas->DrawText(RenderFont, FString::Printf(TEXT("STATE %s"), *GetStateName().ToString()), 4.0f, YPos);
	YPos += YL;

	Super::DisplayDebug(Canvas, DebugDisplay, YL, YPos);
}


void AUTPlayerController::TouchStarted(const ETouchIndex::Type FingerIndex, const FVector Location)
{
	// only fire for first finger down
	if (FingerIndex == 0)
	{
		OnFire();
	}
}

void AUTPlayerController::HearSound(USoundBase* InSoundCue, AActor* SoundPlayer, const FVector& SoundLocation, bool bStopWhenOwnerDestroyed, bool bAmplifyVolume)
{
	bool bIsOccluded = false;
	if (SoundPlayer == this || (GetViewTarget() != NULL && InSoundCue->IsAudible(SoundLocation, GetViewTarget()->GetActorLocation(), (SoundPlayer != NULL) ? SoundPlayer : this, bIsOccluded, true)))
	{
		// we don't want to replicate the location if it's the same as Actor location (so the sound gets played attached to the Actor), but we must if the source Actor isn't relevant
		UNetConnection* Conn = Cast<UNetConnection>(Player);
		FVector RepLoc = (SoundPlayer != NULL && SoundPlayer->GetActorLocation() == SoundLocation && (Conn == NULL || Conn->ActorChannels.Contains(SoundPlayer))) ? FVector::ZeroVector : SoundLocation;
		ClientHearSound(InSoundCue, SoundPlayer, RepLoc, bStopWhenOwnerDestroyed, bIsOccluded, bAmplifyVolume);
	}
}

void AUTPlayerController::ClientHearSound_Implementation(USoundBase* TheSound, AActor* SoundPlayer, FVector SoundLocation, bool bStopWhenOwnerDestroyed, bool bIsOccluded, bool bAmplifyVolume)
{
	if (TheSound != NULL && (SoundPlayer != NULL || !SoundLocation.IsZero()))
	{
		if (SoundPlayer == this || SoundPlayer == GetViewTarget())
		{
			// no attenuation/spatialization, full volume
			FActiveSound NewActiveSound;
			NewActiveSound.World = GetWorld();
			NewActiveSound.Sound = TheSound;

			NewActiveSound.VolumeMultiplier = 1.0f;
			NewActiveSound.PitchMultiplier = 1.0f;

			NewActiveSound.RequestedStartTime = 0.0f;

			NewActiveSound.bLocationDefined = false;
			NewActiveSound.bIsUISound = false;
			NewActiveSound.bHasAttenuationSettings = false;
			NewActiveSound.bAllowSpatialization = false;

			// TODO - Audio Threading. This call would be a task call to dispatch to the audio thread
			if (GEngine->GetAudioDevice() != NULL)
			{
				GEngine->GetAudioDevice()->AddNewActiveSound(NewActiveSound);
			}
		}
		else
		{
			USoundAttenuation* AttenuationOverride = NULL;
			if (bAmplifyVolume)
			{
				// the UGameplayStatics functions copy the FAttenuationSettings by value so no need to create more than one, just reuse
				static USoundAttenuation* OverrideObj = [](){ USoundAttenuation* Result = NewObject<USoundAttenuation>(); Result->AddToRoot(); return Result; }();

				AttenuationOverride = OverrideObj;
				const FAttenuationSettings* DefaultAttenuation = TheSound->GetAttenuationSettingsToApply();
				if (DefaultAttenuation != NULL)
				{
					AttenuationOverride->Attenuation = *DefaultAttenuation;
				}
				// set minimum volume
				// we're assuming that the radius was already checked via HearSound() and thus this won't cause hearing the audio level-wide
				AttenuationOverride->Attenuation.dBAttenuationAtMax = 30.0f;
				// move sound closer
				AActor* ViewTarget = GetViewTarget();
				if (ViewTarget != NULL)
				{
					if (SoundLocation.IsZero())
					{
						SoundLocation = SoundPlayer->GetActorLocation();
					}
					FVector SoundOffset = GetViewTarget()->GetActorLocation() - SoundLocation;
					SoundLocation = SoundLocation + SoundOffset * FMath::Min<float>(SoundOffset.Size() * 0.25f, 2000.0f);
				}
			}
			float VolumeMultiplier = bIsOccluded ? 0.5f : 1.0f;
			if (!SoundLocation.IsZero() && (SoundPlayer == NULL || SoundLocation != SoundPlayer->GetActorLocation()))
			{
				UGameplayStatics::PlaySoundAtLocation(GetWorld(), TheSound, SoundLocation, VolumeMultiplier, 1.0f, 0.0f, AttenuationOverride);
			}
			else if (SoundPlayer != NULL)
			{
				UGameplayStatics::PlaySoundAttached(TheSound, SoundPlayer->GetRootComponent(), NAME_None, FVector::ZeroVector, EAttachLocation::KeepRelativeOffset, bStopWhenOwnerDestroyed, VolumeMultiplier, 1.0f, 0.0f, AttenuationOverride);
			}
		}
	}
}

void AUTPlayerController::CheckDodge(float LastTapTime, float MaxClickTime, bool bForward, bool bBack, bool bLeft, bool bRight)
{
	UUTCharacterMovement* MyCharMovement = UTCharacter ? UTCharacter->UTCharacterMovement : NULL;
	if (MyCharMovement && (bIsHoldingDodge || (GetWorld()->GetTimeSeconds() - LastTapTime < MaxClickTime)))
	{
		MyCharMovement->bPressedDodgeForward = bForward;
		MyCharMovement->bPressedDodgeBack = bBack;
		MyCharMovement->bPressedDodgeLeft = bLeft;
		MyCharMovement->bPressedDodgeRight = bRight;
	}
}

void AUTPlayerController::OnSingleTapDodge()
{
	bRequestedDodge = true;
}

void AUTPlayerController::PerformSingleTapDodge()
{
	UUTCharacterMovement* MyCharMovement = UTCharacter ? UTCharacter->UTCharacterMovement : NULL;
	if (MyCharMovement)
	{
		// base dodge direction on currently pressed axis movement.  
		// If two directions pressed, dodge to the side
		MyCharMovement->bPressedDodgeForward = false;
		MyCharMovement->bPressedDodgeBack = false;
		MyCharMovement->bPressedDodgeLeft = false;
		MyCharMovement->bPressedDodgeRight = false;

		if (MovementStrafeAxis > 0.5f)
		{
			MyCharMovement->bPressedDodgeRight = true;
			UE_LOG(LogUTPlayerController, Verbose, TEXT("SingleTapDodge Right"));
		}
		else if (MovementStrafeAxis < -0.5f)
		{
			MyCharMovement->bPressedDodgeLeft = true;
			UE_LOG(LogUTPlayerController, Verbose, TEXT("SingleTapDodge Left"));
		}
		else if ( MovementForwardAxis >= 0.f)
		{
			MyCharMovement->bPressedDodgeForward = true;
			UE_LOG(LogUTPlayerController, Verbose, TEXT("SingleTapDodge Forward"));
		}
		else
		{
			MyCharMovement->bPressedDodgeBack = true;
			UE_LOG(LogUTPlayerController, Verbose, TEXT("SingleTapDodge Back"));
		}
	}
}

void AUTPlayerController::HoldRollSlide()
{
	bIsHoldingSlideRoll = true;
	UUTCharacterMovement* MyCharMovement = UTCharacter ? UTCharacter->UTCharacterMovement : NULL;
	if (MyCharMovement)
	{
		MyCharMovement->UpdateSlideRoll(true);
	}
}

void AUTPlayerController::ReleaseRollSlide()
{
	bIsHoldingSlideRoll = false;
	UUTCharacterMovement* MyCharMovement = UTCharacter ? UTCharacter->UTCharacterMovement : NULL;
	if (MyCharMovement)
	{
		MyCharMovement->UpdateSlideRoll(bAutoSlide);
	}
}

void AUTPlayerController::HoldDodge()
{
	bIsHoldingDodge = true;
}

void AUTPlayerController::ReleaseDodge()
{
	bIsHoldingDodge = false;
}

void AUTPlayerController::OnTapForward()
{
	LastTapBackTime = -10.f;
	CheckDodge(LastTapForwardTime, MaxDodgeClickTime, true, false, false, false);
	LastTapForwardTime = GetWorld()->GetTimeSeconds();
}

void AUTPlayerController::OnTapBack()
{
	LastTapForwardTime = -10.f;
	CheckDodge(LastTapBackTime, MaxDodgeClickTime, false, true, false, false);
	LastTapBackTime = GetWorld()->GetTimeSeconds();
}

void AUTPlayerController::OnTapLeft()
{
	LastTapRightTime = -10.f;
	CheckDodge(LastTapLeftTime, MaxDodgeClickTime, false, false, true, false);
	LastTapLeftTime = GetWorld()->GetTimeSeconds();
}

void AUTPlayerController::OnTapRight()
{
	LastTapLeftTime = -10.f;
	CheckDodge(LastTapRightTime, MaxDodgeClickTime, false, false, false, true);
	LastTapRightTime = GetWorld()->GetTimeSeconds();
}

void AUTPlayerController::OnTapForwardRelease()
{
	UUTCharacterMovement* MyCharMovement = UTCharacter ? UTCharacter->UTCharacterMovement : NULL;
	if (MyCharMovement && bSingleTapWallDodge && !MyCharMovement->IsMovingOnGround() && (!bSingleTapAfterJump || MyCharMovement->bExplicitJump))
	{
		CheckDodge(LastTapForwardTime, MaxDodgeTapTime, true, false, false, false);
	}
}

void AUTPlayerController::OnTapBackRelease()
{
	UUTCharacterMovement* MyCharMovement = UTCharacter ? UTCharacter->UTCharacterMovement : NULL;
	if (MyCharMovement && bSingleTapWallDodge && !MyCharMovement->IsMovingOnGround() && (!bSingleTapAfterJump || MyCharMovement->bExplicitJump))
	{
		CheckDodge(LastTapBackTime, MaxDodgeTapTime, false, true, false, false);
	}
}

void AUTPlayerController::OnTapLeftRelease()
{
	UUTCharacterMovement* MyCharMovement = UTCharacter ? UTCharacter->UTCharacterMovement : NULL;
	if (MyCharMovement && bSingleTapWallDodge && !MyCharMovement->IsMovingOnGround() && (!bSingleTapAfterJump || MyCharMovement->bExplicitJump))
	{
		CheckDodge(LastTapLeftTime, MaxDodgeTapTime, false, false, true, false);
	}
}

void AUTPlayerController::OnTapRightRelease()
{
	UUTCharacterMovement* MyCharMovement = UTCharacter ? UTCharacter->UTCharacterMovement : NULL;
	if (MyCharMovement && bSingleTapWallDodge && !MyCharMovement->IsMovingOnGround() && (!bSingleTapAfterJump || MyCharMovement->bExplicitJump))
	{
		CheckDodge(LastTapRightTime, MaxDodgeTapTime, false, false, false, true);
	}
}

static void HideComponentTree(const UPrimitiveComponent* Primitive, TSet<FPrimitiveComponentId>& HiddenComponents)
{
	if (Primitive != NULL)
	{
		HiddenComponents.Add(Primitive->ComponentId);
		TArray<USceneComponent*> Children;
		Primitive->GetChildrenComponents(true, Children);
		for (int32 i = 0; i < Children.Num(); i++)
		{
			UPrimitiveComponent* ChildPrim = Cast<UPrimitiveComponent>(Children[i]);
			if (ChildPrim != NULL)
			{
				HiddenComponents.Add(ChildPrim->ComponentId);
			}
		}
	}
}

void AUTPlayerController::UpdateHiddenComponents(const FVector& ViewLocation, TSet<FPrimitiveComponentId>& HiddenComponents)
{
	Super::UpdateHiddenComponents(ViewLocation, HiddenComponents);

	for (int32 i = RecentWeaponPickups.Num() - 1; i >= 0; i--)
	{
		if (RecentWeaponPickups[i] == NULL)
		{
			RecentWeaponPickups.RemoveAt(i, 1, false);
		}
		else if (!RecentWeaponPickups[i]->IsTaken(GetPawn()))
		{
			RecentWeaponPickups[i]->PlayRespawnEffects();
			RecentWeaponPickups.RemoveAt(i, 1, false);
		}
		else if (RecentWeaponPickups[i]->GetMesh() != NULL)
		{
			HiddenComponents.Add(RecentWeaponPickups[i]->GetMesh()->ComponentId);
		}
	}

	// hide all components that shouldn't be shown in the current 1P/3P state
	// with bOwnerNoSee/bOnlyOwnerSee not being propagated to children this method is much easier to maintain
	// although less efficient
	// TODO: evaluate performance
	AUTCharacter* P = Cast<AUTCharacter>(GetViewTarget());
	if (IsBehindView())
	{
		// hide first person weapon
		if (P != NULL && P->GetWeapon() != NULL)
		{
			HideComponentTree(P->GetWeapon()->Mesh, HiddenComponents);
		}
	}
	else if (P != NULL)
	{
		// hide third person character model
		HideComponentTree(P->GetMesh(), HiddenComponents);
	}
	else if (GetViewTarget() != NULL)
	{
		// for others we can't just hide everything because we don't know where the camera component is and we don't want to hide its attachments
		// so just hide root
		UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(GetViewTarget()->GetRootComponent());
		if (RootPrim != NULL)
		{
			HiddenComponents.Add(RootPrim->ComponentId);
		}
	}

	// hide other local players' first person weapons
	for (FLocalPlayerIterator It(GEngine, GetWorld()); It; ++It)
	{
		if (It->PlayerController != this && It->PlayerController != NULL)
		{
			AUTCharacter* OtherP = Cast<AUTCharacter>(It->PlayerController->GetViewTarget());
			if (OtherP != NULL && OtherP->GetWeapon() != NULL)
			{
				HideComponentTree(OtherP->GetWeapon()->Mesh, HiddenComponents);
			}
		}
	}
}

void AUTPlayerController::SetName(const FString& S)
{
	if (!S.IsEmpty())
	{
		Super::SetName(S);

		UUTLocalPlayer* LP = Cast<UUTLocalPlayer>(Player);
		if (LP != NULL)
		{
			LP->SetNickname(S);
			LP->SaveProfileSettings();
		}
	}
}

void AUTPlayerController::ToggleScoreboard(bool bShow)
{
	if (MyHUD != NULL && Cast<AUTHUD>(MyHUD) != NULL)
	{
		Cast<AUTHUD>(MyHUD)->ToggleScoreboard(bShow);
	}
}

void AUTPlayerController::ClientToggleScoreboard_Implementation(bool bShow)
{
	ToggleScoreboard(bShow);
}

void AUTPlayerController::ClientSetHUDAndScoreboard_Implementation(TSubclassOf<class AHUD> NewHUDClass, TSubclassOf<class UUTScoreboard> NewScoreboardClass)
{
	// First, create the HUD

	ClientSetHUD_Implementation(NewHUDClass);

	MyUTHUD = Cast<AUTHUD>(MyHUD);
	if (MyUTHUD != NULL && NewScoreboardClass != NULL)
	{
		MyUTHUD->CreateScoreboard(NewScoreboardClass);
	}
	
}

void AUTPlayerController::OnShowScores()
{
	ToggleScoreboard(true);
}

void AUTPlayerController::OnHideScores()
{
	ToggleScoreboard(false);
}

AUTCharacter* AUTPlayerController::GetUTCharacter()
{
	return UTCharacter;
}

void AUTPlayerController::ServerRestartPlayer_Implementation()
{
	if (!GetWorld()->GetAuthGameMode()->HasMatchStarted() && UTPlayerState != NULL)
	{
		UTPlayerState ->bReadyToPlay = true;
	}

	// If we can't restart this player, try to view a new player
	if (!CanRestartPlayer())
	{
		ServerViewNextPlayer();
		return;
	}

	Super::ServerRestartPlayer_Implementation();

}

bool AUTPlayerController::CanRestartPlayer()
{
	AUTGameMode* GM = Cast<AUTGameMode>(GetWorld()->GetAuthGameMode());
	if (GM && GM->RestrictPlayerSpawns()) return false;

	return Super::CanRestartPlayer() && UTPlayerState->RespawnTime <= 0.0f;
}


void AUTPlayerController::BehindView(bool bWantBehindView)
{
	if (IsInState(NAME_Spectating))
	{
		bSpectateBehindView = bWantBehindView;
	}
	SetCameraMode(bWantBehindView ? FName(TEXT("FreeCam")) : FName(TEXT("Default")));
}

bool AUTPlayerController::IsBehindView()
{
	if (PlayerCameraManager != NULL)
	{
		static FName NAME_FreeCam(TEXT("FreeCam"));

		AUTPlayerCameraManager* UTCam = Cast<AUTPlayerCameraManager>(PlayerCameraManager);
		FName CameraStyle = (UTCam != NULL) ? UTCam->GetCameraStyleWithOverrides() : PlayerCameraManager->CameraStyle;

		return CameraStyle == NAME_FreeCam;
	}
	else
	{
		return false;
	}
}

void AUTPlayerController::ClientSetCameraMode_Implementation( FName NewCamMode )
{
	if (PlayerCameraManager)
	{
		PlayerCameraManager->CameraStyle = NewCamMode;
	}
}

void AUTPlayerController::SetCameraMode( FName NewCamMode )
{
	ClientSetCameraMode_Implementation(NewCamMode);
	
	if ( GetNetMode() == NM_DedicatedServer )
	{
		ClientSetCameraMode( NewCamMode );
	}
}

void AUTPlayerController::ClientGameEnded_Implementation(AActor* EndGameFocus, bool bIsWinner)
{
	ChangeState(FName(TEXT("GameOver")));
	FinalViewTarget = EndGameFocus;
	BehindView(true);
	Super::ClientGameEnded_Implementation(EndGameFocus, bIsWinner);

	// free all Pawns locally
	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		if (It->IsValid())
		{
			It->Get()->TurnOff();
		}
	}
}

void AUTPlayerController::SetViewTarget(class AActor* NewViewTarget, FViewTargetTransitionParams TransitionParams)
{
	// remove any FOV shifts when changing targets (e.g. sniper zoom)
	if (PlayerCameraManager != NULL)
	{
		PlayerCameraManager->UnlockFOV();
	}

	if (FinalViewTarget != NULL)
	{
		NewViewTarget = FinalViewTarget;
	}

	AUTViewPlaceholder *UTPlaceholder = Cast<AUTViewPlaceholder>(GetViewTarget());
	
	Super::SetViewTarget(NewViewTarget, TransitionParams);

	// See if we're no longer viewing a placeholder and destroy it
	if (UTPlaceholder != nullptr && GetViewTarget() != UTPlaceholder)
	{
		UTPlaceholder->Destroy();
	}
}

void AUTPlayerController::ServerViewSelf_Implementation(FViewTargetTransitionParams TransitionParams)
{
	if (IsInState(NAME_Spectating))
	{
		FVector CurrentViewLoc;
		FRotator CurrentViewRot;
		GetPlayerViewPoint(CurrentViewLoc, CurrentViewRot);
		AActor* NewViewTarget = (GetSpectatorPawn() != NULL) ? GetSpectatorPawn() : SpawnSpectatorPawn();
		if (NewViewTarget == NULL)
		{
			NewViewTarget = this;
		}
		// move spectator pawn to current view location
		NewViewTarget->SetActorLocationAndRotation(CurrentViewLoc, CurrentViewRot);
		ResetCameraMode();
		SetViewTarget(NewViewTarget, TransitionParams);
		ClientSetViewTarget(NewViewTarget, TransitionParams);
	}
}

void AUTPlayerController::ClientHalftime_Implementation()
{
	// Freeze all of the pawns
	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		if (*It)
		{
			(*It)->TurnOff();
		}
	}
}

void AUTPlayerController::TestResult(uint16 ButtonID)
{
}


void AUTPlayerController::Possess(APawn* PawnToPossess)
{
	Super::Possess(PawnToPossess);

	if (UTPlayerState->bHasHighScore)
	{
		AUTCharacter *UTChar = Cast<AUTCharacter>(GetPawn());
		if (UTChar != nullptr)
		{
			UTChar->bHasHighScore = true;
			UTChar->HasHighScoreChanged();
		}
	}
}

void AUTPlayerController::PawnLeavingGame()
{
	if (UTCharacter != NULL)
	{
		UTCharacter->PlayerSuicide();
	}
	// TODO: vehicles
	else
	{
		UnPossess();
	}
}

void AUTPlayerController::PlayerTick( float DeltaTime )
{
	Super::PlayerTick(DeltaTime);
	if (StateName == FName(TEXT("GameOver")))
	{
		UpdateRotation(DeltaTime);
	}
	// if we have no UTCharacterMovement, we need to apply firing here since it won't happen from the component
	if (GetPawn() == NULL || Cast<UUTCharacterMovement>(GetPawn()->GetMovementComponent()) == NULL)
	{
		ApplyDeferredFireInputs();
	}

	// Follow the last spectated player again when they respawn
	if (StateName == NAME_Spectating)
	{
		APawn* ViewTargetPawn = PlayerCameraManager->GetViewTargetPawn();
		AUTCharacter* ViewTargetCharacter = Cast<AUTCharacter>(ViewTargetPawn);
		if (ViewTargetPawn == nullptr || (ViewTargetCharacter != nullptr && ViewTargetCharacter->IsDead()))
		{
			if (LastSpectatedPlayerState != nullptr)
			{
				for (FConstPawnIterator Iterator = GetWorld()->GetPawnIterator(); Iterator; ++Iterator)
				{
					APawn* Pawn = *Iterator;
					if (Pawn != nullptr && Pawn->PlayerState == LastSpectatedPlayerState)
					{
						ServerViewPawn(*Iterator);
					}
				}
			}
		}
	}
}

void AUTPlayerController::NotifyTakeHit(AController* InstigatedBy, int32 Damage, FVector Momentum, const FDamageEvent& DamageEvent)
{
	APlayerState* InstigatedByState = (InstigatedBy != NULL) ? InstigatedBy->PlayerState : NULL;
	FVector RelHitLocation(FVector::ZeroVector);
	if (DamageEvent.IsOfType(FPointDamageEvent::ClassID))
	{
		RelHitLocation = ((FPointDamageEvent*)&DamageEvent)->HitInfo.Location - GetPawn()->GetActorLocation();
	}
	else if (DamageEvent.IsOfType(FRadialDamageEvent::ClassID) && ((FRadialDamageEvent*)&DamageEvent)->ComponentHits.Num() > 0)
	{
		RelHitLocation = ((FRadialDamageEvent*)&DamageEvent)->ComponentHits[0].Location - GetPawn()->GetActorLocation();
	}
	ClientNotifyTakeHit(InstigatedByState, Damage, Momentum, RelHitLocation, DamageEvent.DamageTypeClass);
}

void AUTPlayerController::ClientNotifyTakeHit_Implementation(APlayerState* InstigatedBy, int32 Damage, FVector Momentum, FVector RelHitLocation, TSubclassOf<UDamageType> DamageType)
{
	if (MyUTHUD != NULL)
	{
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		MyUTHUD->PawnDamaged(((GetPawn() != NULL) ? GetPawn()->GetActorLocation() : FVector::ZeroVector) + RelHitLocation, Damage, DamageType, InstigatedBy != PlayerState && GS != NULL && GS->OnSameTeam(InstigatedBy, this));
	}
}

void AUTPlayerController::ClientNotifyCausedHit_Implementation(APawn* HitPawn, int32 Damage)
{
	// by default we only show HUD hitconfirms for hits that the player could conceivably see (i.e. target is in LOS)
	if (HitPawn != NULL && HitPawn->GetRootComponent() != NULL && GetPawn() != NULL && MyUTHUD != NULL)
	{
		float VictimLastRenderTime = -1.0f;
		TArray<USceneComponent*> Components;
		HitPawn->GetRootComponent()->GetChildrenComponents(true, Components);
		for (int32 i = 0; i < Components.Num(); i++)
		{
			UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Components[i]);
			if (Prim != NULL)
			{
				VictimLastRenderTime = FMath::Max<float>(VictimLastRenderTime, Prim->LastRenderTime);
			}
		}
		if (GetWorld()->TimeSeconds - VictimLastRenderTime < 0.15f)
		{
			MyUTHUD->CausedDamage(HitPawn, Damage);
		}
	}
}


void AUTPlayerController::K2_ReceiveLocalizedMessage(TSubclassOf<ULocalMessage> Message, int32 Switch, APlayerState* RelatedPlayerState_1, APlayerState* RelatedPlayerState_2, UObject* OptionalObject)
{
	ClientReceiveLocalizedMessage(Message, Switch, RelatedPlayerState_1, RelatedPlayerState_2, OptionalObject);
}

void AUTPlayerController::ChangeTeam(uint8 NewTeamIndex)
{
	AUTPlayerState* PS = Cast<AUTPlayerState>(PlayerState);
	if (PS != NULL)
	{
		PS->ServerRequestChangeTeam(NewTeamIndex);
	}
}

void AUTPlayerController::Suicide()
{
	ServerSuicide();
}
void AUTPlayerController::ServerSuicide_Implementation()
{
	// throttle suicides to avoid spamming to grief own team in TDM
	if (GetPawn() != NULL && (GetWorld()->TimeSeconds - GetPawn()->CreationTime > 10.0f || GetWorld()->WorldType == EWorldType::PIE || GetNetMode() == NM_Standalone))
	{
		AUTCharacter* Char = Cast<AUTCharacter>(GetPawn());
		if (Char != NULL)
		{
			Char->PlayerSuicide();
		}
	}
}
bool AUTPlayerController::ServerSuicide_Validate()
{
	return true;
}


void AUTPlayerController::Emote(int32 EmoteIndex)
{
	if (GetWorld()->GetRealTimeSeconds() - LastEmoteTime > EmoteCooldownTime)
	{
		ServerEmote(EmoteIndex);
		LastEmoteTime = GetWorld()->GetRealTimeSeconds();
	}
}

void AUTPlayerController::PlayEmote1()
{
	UUTGameUserSettings* GS = Cast<UUTGameUserSettings>(GEngine->GetGameUserSettings());
	if (GS)
	{
		Emote(GS->GetEmoteIndex1());
	}
}

void AUTPlayerController::PlayEmote2()
{
	UUTGameUserSettings* GS = Cast<UUTGameUserSettings>(GEngine->GetGameUserSettings());
	if (GS)
	{
		Emote(GS->GetEmoteIndex2());
	}
}

void AUTPlayerController::PlayEmote3()
{
	UUTGameUserSettings* GS = Cast<UUTGameUserSettings>(GEngine->GetGameUserSettings());
	if (GS)
	{
		Emote(GS->GetEmoteIndex3());
	}
}

bool AUTPlayerController::ServerEmote_Validate(int32 EmoteIndex)
{
	return true;
}

void AUTPlayerController::ServerEmote_Implementation(int32 EmoteIndex)
{
	if (UTCharacter != nullptr)
	{
		UTCharacter->PlayEmote(EmoteIndex);
	}
}

void AUTPlayerController::ReceivedPlayer()
{
	Super::ReceivedPlayer();

	UUTLocalPlayer* LP = Cast<UUTLocalPlayer>(Player);
	
	if (LP != NULL && GetWorld()->GetNetMode() != NM_Standalone)
	{
		if (FUTAnalytics::IsAvailable() && (GetWorld()->GetNetMode() != NM_Client || GetWorld()->GetNetDriver() != NULL)) // make sure we don't do analytics for demo playback
		{
			FString ServerInfo = (GetWorld()->GetNetMode() == NM_Client) ? GetWorld()->GetNetDriver()->ServerConnection->URL.ToString() : GEngine->GetWorldContextFromWorldChecked(GetWorld()).LastURL.ToString();
			FUTAnalytics::GetProvider().RecordEvent(TEXT("PlayerConnect"), TEXT("Server"), ServerInfo);
		}
	}
}

void AUTPlayerController::ApplyDeferredFireInputs()
{
	for (FDeferredFireInput& Input : DeferredFireInputs)
	{
		if (Input.bStartFire)
		{
			if (UTCharacter != NULL)
			{
				if (StateName == NAME_Playing)
				{
					UTCharacter->StartFire(Input.FireMode);
				}
			}
			else if (GetPawn() != nullptr)
			{
				GetPawn()->PawnStartFire(Input.FireMode);
			}
		}
		else if (UTCharacter != NULL)
		{
			UTCharacter->StopFire(Input.FireMode);
		}
	}
	DeferredFireInputs.Empty();
}

bool AUTPlayerController::HasDeferredFireInputs()
{
	for (FDeferredFireInput& Input : DeferredFireInputs)
	{
		if (Input.bStartFire)
		{
			return true;
		}
	}
	return false;
}

void AUTPlayerController::SetEmoteSpeed(float NewEmoteSpeed)
{
	if (UTCharacter != nullptr)
	{
		UTCharacter->ServerSetEmoteSpeed(NewEmoteSpeed);
	}
}

void AUTPlayerController::FasterEmote()
{
	if (UTCharacter != nullptr && UTCharacter->EmoteCount > 0)
	{
		UTCharacter->ServerFasterEmote();
	}
}

void AUTPlayerController::SlowerEmote()
{
	if (UTCharacter != nullptr && UTCharacter->EmoteCount > 0)
	{
		UTCharacter->ServerSlowerEmote();
	}
}

void AUTPlayerController::ClientSetViewTarget_Implementation(AActor* A, FViewTargetTransitionParams TransitionParams)
{
	if (StateName == NAME_Spectating)
	{
		AUTCharacter* Char = Cast<AUTCharacter>(A);
		if (Char)
		{
			LastSpectatedCharacter = Char;
			LastSpectatedPlayerState = Char->PlayerState;
		}
	}
	if (PlayerCameraManager != NULL)
	{
		PlayerCameraManager->UnlockFOV();
	}

	Super::ClientSetViewTarget_Implementation(A, TransitionParams);
}

bool AUTPlayerController::ServerViewPawn_Validate(APawn* PawnToView)
{
	return true;
}

void AUTPlayerController::ServerViewPawn_Implementation(APawn* PawnToView)
{
	// Don't view other pawns when we already have a pawn
	if (GetPawn() != nullptr)
	{
		return;
	}

	if (PawnToView)
	{
		SetViewTarget(PawnToView->PlayerState);
	}
}

void AUTPlayerController::SetMouseSensitivityUT(float NewSensitivity)
{
	PlayerInput->SetMouseSensitivity(NewSensitivity);

	UInputSettings* InputSettings = UInputSettings::StaticClass()->GetDefaultObject<UInputSettings>();
	for (FInputAxisConfigEntry& Entry : InputSettings->AxisConfig)
	{
		if (Entry.AxisKeyName == EKeys::MouseX || Entry.AxisKeyName == EKeys::MouseY)
		{
			Entry.AxisProperties.Sensitivity = NewSensitivity;
		}
	}

	InputSettings->SaveConfig();
}

float AUTPlayerController::GetWeaponAutoSwitchPriority(FString WeaponClassname, float DefaultPriority)
{
	if (Cast<UUTLocalPlayer>(Player))
	{
		UUTProfileSettings* ProfileSettings = Cast<UUTLocalPlayer>(Player)->GetProfileSettings();
		if (ProfileSettings)
		{
			return ProfileSettings->GetWeaponPriority(WeaponClassname, DefaultPriority);
		}
	}
	return DefaultPriority;
}

void AUTPlayerController::RconAuth(FString Password)
{
	ServerRconAuth(Password);
}

bool AUTPlayerController::ServerRconAuth_Validate(const FString& Password)
{
	return true;
}

void AUTPlayerController::ServerRconAuth_Implementation(const FString& Password)
{
	if (UTPlayerState != nullptr && !UTPlayerState->bIsRconAdmin && !GetDefault<UUTGameEngine>()->RconPassword.IsEmpty())
	{
		if (GetDefault<UUTGameEngine>()->RconPassword == Password)
		{
			ClientSay(UTPlayerState, TEXT("Rcon authenticated!"), ChatDestinations::System);
			UTPlayerState->bIsRconAdmin = true;
		}
		else
		{
			ClientSay(UTPlayerState, TEXT("Rcon password incorrect"), ChatDestinations::System);
		}
	}
	else
	{
		ClientSay(UTPlayerState, TEXT("Rcon password unset"), ChatDestinations::System);
	}
}

void AUTPlayerController::RconMap(FString NewMap)
{
	ServerRconMap(NewMap);
}

bool AUTPlayerController::ServerRconMap_Validate(const FString& NewMap)
{
	return true;
}

void AUTPlayerController::ServerRconMap_Implementation(const FString& NewMap)
{
	if (UTPlayerState != nullptr && UTPlayerState->bIsRconAdmin)
	{
		FString MapFullName;
		if (FPackageName::SearchForPackageOnDisk(NewMap + FPackageName::GetMapPackageExtension(), &MapFullName))
		{
			GetWorld()->ServerTravel(MapFullName, false);
		}
		else
		{
			ClientSay(UTPlayerState, FString::Printf(TEXT("Rcon %s doesn't exist"), *NewMap), ChatDestinations::System);
		}
	}
	else
	{
		ClientSay(UTPlayerState, TEXT("Rcon not authenticated"), ChatDestinations::System);
	}
}

void AUTPlayerController::RconNextMap(FString NextMap)
{
	ServerRconNextMap(NextMap);
}

bool AUTPlayerController::ServerRconNextMap_Validate(const FString& NextMap)
{
	return true;
}

void AUTPlayerController::ServerRconNextMap_Implementation(const FString& NextMap)
{
	if (UTPlayerState != nullptr && UTPlayerState->bIsRconAdmin)
	{
		AUTGameMode* Game = GetWorld()->GetAuthGameMode<AUTGameMode>();
		if (Game != nullptr)
		{
			FString MapFullName;
			if (FPackageName::SearchForPackageOnDisk(NextMap + FPackageName::GetMapPackageExtension(), &MapFullName))
			{
				Game->RconNextMapName = MapFullName;
				ClientSay(UTPlayerState, FString::Printf(TEXT("Next map set to %s"), *NextMap), ChatDestinations::System);
			}
			else
			{
				ClientSay(UTPlayerState, FString::Printf(TEXT("Rcon %s doesn't exist"), *NextMap), ChatDestinations::System);
			}
		}
	}
	else
	{
		ClientSay(UTPlayerState, TEXT("Rcon not authenticated"), ChatDestinations::System);
	}
}

void AUTPlayerController::BeginInactiveState()
{
	Super::BeginInactiveState();

	AGameState const* const GameState = GetWorld()->GameState;

	GetWorldTimerManager().SetTimer(this, &AUTPlayerController::SpectateKiller, KillerSpectateDelay);
}

void AUTPlayerController::EndInactiveState()
{
	Super::EndInactiveState();

	GetWorldTimerManager().ClearTimer(this, &AUTPlayerController::SpectateKiller);
}

void AUTPlayerController::SpectateKiller()
{
	/*
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS != nullptr && GS->bViewKillerOnDeath && UTPlayerState->LastKillerPlayerState != nullptr && UTPlayerState->LastKillerPlayerState != UTPlayerState)
	{
		for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
		{
			AUTCharacter *UTChar = Cast<AUTCharacter>(*It);
			if (UTChar != nullptr && UTChar->PlayerState == UTPlayerState->LastKillerPlayerState)
			{
				ServerViewPlaceholderAtLocation(UTChar->GetActorLocation() + FVector(0, 0, UTChar->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight()));
			}
		}
	}
	*/
}

void AUTPlayerController::ServerViewPlaceholderAtLocation_Implementation(FVector Location)
{
	if (GetPawn() == NULL && (IsInState(NAME_Spectating) || IsInState(NAME_Inactive)))
	{
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.bNoCollisionFail = true;
		AUTViewPlaceholder *ViewPlaceholder = GetWorld()->SpawnActor<AUTViewPlaceholder>(AUTViewPlaceholder::StaticClass(), Location, FRotator(), Params);
		SetViewTarget(ViewPlaceholder);
	}
}

bool AUTPlayerController::ServerViewPlaceholderAtLocation_Validate(FVector Location)
{
	return true;
}


void AUTPlayerController::UTBugIt(const FString& ScreenShotDescription)
{
	ConsoleCommand(FString::Printf(TEXT("BUGSCREENSHOTWITHHUDINFO %s"), *ScreenShotDescription));

	FVector ViewLocation;
	FRotator ViewRotation;
	GetPlayerViewPoint(ViewLocation, ViewRotation);

	if (GetPawn() != NULL)
	{
		ViewLocation = GetPawn()->GetActorLocation();
	}

	FString GoString, LocString;
	UTBugItStringCreator(ViewLocation, ViewRotation, GoString, LocString);

	UTLogOutBugItGoToLogFile(ScreenShotDescription, GoString, LocString);
}

void AUTPlayerController::UTBugItStringCreator(FVector ViewLocation, FRotator ViewRotation, FString& GoString, FString& LocString)
{
	GoString = FString::Printf(TEXT("BugItGo %f %f %f %f %f %f"), ViewLocation.X, ViewLocation.Y, ViewLocation.Z, ViewRotation.Pitch, ViewRotation.Yaw, ViewRotation.Roll);
	UE_LOG(LogUTPlayerController, Log, TEXT("%s"), *GoString);

	LocString = FString::Printf(TEXT("?BugLoc=%s?BugRot=%s"), *ViewLocation.ToString(), *ViewRotation.ToString());
	UE_LOG(LogUTPlayerController, Log, TEXT("%s"), *LocString);
}

void AUTPlayerController::UTLogOutBugItGoToLogFile(const FString& InScreenShotDesc, const FString& InGoString, const FString& InLocString)
{
#if ALLOW_DEBUG_FILES
	// Create folder if not already there

	const FString OutputDir = FPaths::BugItDir() + InScreenShotDesc + TEXT("/");

	IFileManager::Get().MakeDirectory(*OutputDir);
	// Create archive for log data.
	// we have to +1 on the GScreenshotBitmapIndex as it will be incremented by the bugitscreenshot which is processed next tick

	const FString DescPlusExtension = FString::Printf(TEXT("%s%i.txt"), *InScreenShotDesc, GScreenshotBitmapIndex);
	const FString TxtFileName = CreateProfileFilename(DescPlusExtension, false);

	//FString::Printf( TEXT("BugIt%s-%s%05i"), *GEngineVersion.ToString(), *InScreenShotDesc, GScreenshotBitmapIndex+1 ) + TEXT( ".txt" );
	const FString FullFileName = OutputDir + TxtFileName;

	FOutputDeviceFile OutputFile(*FullFileName);
	//FArchive* OutputFile = IFileManager::Get().CreateDebugFileWriter( *(FullFileName), FILEWRITE_Append );


	OutputFile.Logf(TEXT("Dumping BugIt data chart at %s using build %s built from changelist %i"), *FDateTime::Now().ToString(), *GEngineVersion.ToString(), GetChangeListNumberForPerfTesting());

	const FString MapNameStr = GetWorld()->GetMapName();

	OutputFile.Logf(TEXT("MapName: %s"), *MapNameStr);

	OutputFile.Logf(TEXT("Description: %s"), *InScreenShotDesc);
	OutputFile.Logf(TEXT("%s"), *InGoString);
	OutputFile.Logf(TEXT("%s"), *InLocString);
	
	// Flush, close and delete.
	//delete OutputFile;
	OutputFile.TearDown();

	// so here we want to send this bad boy back to the PC
	SendDataToPCViaUnrealConsole(TEXT("UE_PROFILER!BUGIT:"), *(FullFileName));
#endif // ALLOW_DEBUG_FILES
}

void AUTPlayerController::ClientSetLocation_Implementation(FVector NewLocation, FRotator NewRotation)
{
	Super::ClientSetLocation_Implementation(NewLocation, NewRotation);
	if (!GetPawn())
	{
		SetSpawnLocation(NewLocation);
	}
}
