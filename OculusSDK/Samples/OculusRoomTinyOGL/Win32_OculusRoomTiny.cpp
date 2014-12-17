/************************************************************************************

Filename    :   Win32_OculusRoomTiny.cpp
Content     :   First-person view test application for Oculus Rift
Created     :   November 8, 2014
Authors     :   Federico Mammano, from the original code written by: 
				Michael Antonov, Andrew Reisse, Tom Heath, Volga Aksoy

Copyright   :   Copyright 2012 Oculus, Inc. All Rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*************************************************************************************/

//-------------------------------------------------------------------------------------
// This app renders a simple flat-shaded room allowing the user to move along the 
// floor and look around with an HMD and mouse/keyboard. 
// The following keys work:
//  'W', 'S', 'A', 'D', 'F' - Move forward, back; strafe left/right, toggle freeze in timewarp.
// The world right handed coordinate system is defined as  Y -> Up, Z -> Back, X -> Right

// Include the OculusVR SDK
#include "OVR_CAPI.h"

// Choose whether the SDK performs rendering/distortion, or the application. 
#define          SDK_RENDER 1  //Do NOT switch until you have viewed and understood the Health and Safety message.
                               //Disabling this makes it a non-compliant app, and not suitable for demonstration. In place for development only.
const bool       FullScreen = true; //Should be true for correct timing.  Use false for debug only.


// Include Non-SDK supporting Utilities from other files
#include "RenderTiny_OGL_Device.h"
HWND Util_InitWindowAndGraphics    (Recti vp, int fullscreen, int multiSampleCount, bool UseAppWindowFrame, RenderDevice ** pDevice);
void Util_ReleaseWindowAndGraphics (RenderDevice* pRender);
bool Util_RespondToControls        (float & EyeYaw, Vector3f & EyePos, Quatf PoseOrientation);
void PopulateRoomScene             (Scene* scene, RenderDevice* render);

//Structures for the application
ovrHmd             HMD;
ovrEyeRenderDesc   EyeRenderDesc[2];
ovrRecti           EyeRenderViewport[2];
RenderDevice*      pRender = 0;
Texture*           pRendertargetTexture = 0;
Scene*             pRoomScene = 0;

// Specifics for whether the SDK or the APP is doing the distortion.
#if SDK_RENDER
	#include "OVR_CAPI_GL.h"
	ovrGLTexture		EyeTexture[2];
#else
	ShaderSet *         Shaders;  
	Ptr<Buffer>         MeshVBs[2];
	Ptr<Buffer>         MeshIBs[2]; 
	ovrVector2f         UVScaleOffset[2][2];
#endif
   
//-------------------------------------------------------------------------------------
int Init()
{
    // Initializes LibOVR, and the Rift
    ovr_Initialize();
    if (!HMD)
    {
        HMD = ovrHmd_Create(0);
        if (!HMD)
        {
            MessageBoxA(NULL, "Oculus Rift not detected.", "", MB_OK);
            return(1);
        }
        if (HMD->ProductName[0] == '\0')
            MessageBoxA(NULL, "Rift detected, display not enabled.", "", MB_OK);
    }

	//Setup Window and Graphics - use window frame if relying on Oculus driver
	const int backBufferMultisample = 1;
    bool UseAppWindowFrame = (HMD->HmdCaps & ovrHmdCap_ExtendDesktop) ? false : true;
    HWND window = Util_InitWindowAndGraphics(Recti(HMD->WindowsPos, HMD->Resolution),
                                         FullScreen, backBufferMultisample, UseAppWindowFrame,&pRender);
	if (!window) return 1;
	ovrHmd_AttachToWindow(HMD, window, NULL, NULL);

    //Configure Stereo settings.
    Sizei recommenedTex0Size = ovrHmd_GetFovTextureSize(HMD, ovrEye_Left,  HMD->DefaultEyeFov[0], 1.0f);
    Sizei recommenedTex1Size = ovrHmd_GetFovTextureSize(HMD, ovrEye_Right, HMD->DefaultEyeFov[1], 1.0f);
	Sizei RenderTargetSize;
    RenderTargetSize.w = recommenedTex0Size.w + recommenedTex1Size.w;
    RenderTargetSize.h = max ( recommenedTex0Size.h, recommenedTex1Size.h );

    const int eyeRenderMultisample = 1;
    pRendertargetTexture = pRender->CreateTexture(Texture_RGBA | Texture_RenderTarget |
                                                  eyeRenderMultisample,
                                                  RenderTargetSize.w, RenderTargetSize.h, NULL);
    // The actual RT size may be different due to HW limits.
    RenderTargetSize.w = pRendertargetTexture->GetWidth();
    RenderTargetSize.h = pRendertargetTexture->GetHeight();

    // Initialize eye rendering information.
    // The viewport sizes are re-computed in case RenderTargetSize changed due to HW limitations.
    ovrFovPort eyeFov[2] = { HMD->DefaultEyeFov[0], HMD->DefaultEyeFov[1] } ;

    EyeRenderViewport[0].Pos  = Vector2i(0,0);
    EyeRenderViewport[0].Size = Sizei(RenderTargetSize.w / 2, RenderTargetSize.h);
    EyeRenderViewport[1].Pos  = Vector2i((RenderTargetSize.w + 1) / 2, 0);
    EyeRenderViewport[1].Size = EyeRenderViewport[0].Size;

    #if SDK_RENDER
	// Query OGL texture data.
	EyeTexture[0].OGL.Header.API			= ovrRenderAPI_OpenGL;
	EyeTexture[0].OGL.Header.TextureSize	= RenderTargetSize;
	EyeTexture[0].OGL.Header.RenderViewport = EyeRenderViewport[0];
	EyeTexture[0].OGL.TexId					= pRendertargetTexture->TexId;

    // Right eye uses the same texture, but different rendering viewport.
    EyeTexture[1]							= EyeTexture[0];
    EyeTexture[1].OGL.Header.RenderViewport	= EyeRenderViewport[1];

    // Configure OpenGL.
    ovrGLConfig oglcfg;
    oglcfg.OGL.Header.API					= ovrRenderAPI_OpenGL;
    oglcfg.OGL.Header.RTSize				= Sizei(HMD->Resolution.w, HMD->Resolution.h);
    oglcfg.OGL.Header.Multisample			= backBufferMultisample;
	oglcfg.OGL.Window						= window;
	oglcfg.OGL.DC							= GetDC(window);

    if (!ovrHmd_ConfigureRendering(HMD, &oglcfg.Config,
		                           ovrDistortionCap_Chromatic | ovrDistortionCap_Vignette |
                                   ovrDistortionCap_TimeWarp | ovrDistortionCap_Overdrive,
								   eyeFov, EyeRenderDesc))	return(1);	

    #else
	//Distortion vertex shader
	const char* vertexShader =
		"#version 110																			\n"
		"uniform vec2 EyeToSourceUVScale;														\n"
		"uniform vec2 EyeToSourceUVOffset;														\n"
		"uniform mat4 EyeRotationStart;															\n"
		"uniform mat4 EyeRotationEnd;															\n"
		"attribute vec2 Position;																\n"
		"attribute vec2 inTWLF_V;																\n"		
		"attribute vec2 inTexCoord0;															\n"
		"attribute vec2 inTexCoord1;															\n"
		"attribute vec2 inTexCoord2;															\n"
		"varying vec4 oPosition;																\n"
		"varying vec2 oTexCoord0;																\n"
		"varying vec2 oTexCoord1;																\n"
		"varying vec2 oTexCoord2;																\n"
		"varying float oVignette;																\n"
		"vec2 TexCoord0 = vec2(inTexCoord0.x, -inTexCoord0.y);									\n"
		"vec2 TexCoord1 = vec2(inTexCoord1.x, -inTexCoord1.y);									\n"
		"vec2 TexCoord2 = vec2(inTexCoord2.x, -inTexCoord2.y);									\n"
		"float timewarpLerpFactor = inTWLF_V.x;													\n"
		"float Vignette = inTWLF_V.y;															\n"
		"vec2 TimewarpTexCoord( in vec2 TexCoord, in mat4 rotMat )								\n"
		"{																						\n"
		// Vertex inputs are in TanEyeAngle space for the R,G,B channels (i.e. after chromatic 
		// aberration and distortion). These are now "real world" vectors in direction (x,y,1) 
		// relative to the eye of the HMD.	Apply the 3x3 timewarp rotation to these vectors.
		"   vec3 transformed = vec3( ( rotMat * vec4( TexCoord.xy , 1.00000, 1.00000) ).xyz );	\n"
		// Project them back onto the Z=1 plane of the rendered images.
		"   vec2 flattened = (transformed.xy  / transformed.z );								\n"
		// Scale them into ([0,0.5],[0,1]) or ([0.5,0],[0,1]) UV lookup space (depending on eye)
		"   return ((EyeToSourceUVScale * flattened) + EyeToSourceUVOffset);					\n"
		"}																						\n"
		"mat4 mat4_lerp( in mat4 x, in mat4 y, in mat4 s )										\n"
		"{																						\n"
		"	return mat4(mix(x[0],y[0],s[0]), mix(x[1],y[1],s[1]), mix(x[2],y[2],s[2]), mix(x[3],y[3],s[3]));\n"
		"}																						\n"
		"void main()																			\n"
		"{																						\n"
		"   mat4 lerpedEyeRot = mat4_lerp( EyeRotationStart, EyeRotationEnd, mat4( timewarpLerpFactor));\n"
		"   oTexCoord0 = TimewarpTexCoord( TexCoord0, lerpedEyeRot);							\n"
		"   oTexCoord1 = TimewarpTexCoord( TexCoord1, lerpedEyeRot);							\n"
		"   oTexCoord2 = TimewarpTexCoord( TexCoord2, lerpedEyeRot);							\n"
		"   oPosition = vec4( Position.xy , 0.500000, 1.00000);									\n"
		"   oVignette = Vignette;																\n"
		"   gl_Position = oPosition;															\n"
		"}";

	//Distortion pixel shader
	const char* pixelShader = 
		"#version 110																			\n"
		"uniform sampler2D Texture0;															\n"
		"varying vec4 oPosition;																\n"
		"varying vec2 oTexCoord0;																\n"
		"varying vec2 oTexCoord1;																\n"
		"varying vec2 oTexCoord2;																\n"
		"varying float oVignette;																\n"
		"void main()																			\n"
		"{																						\n"
		// 3 samples for fixing chromatic aberrations
		"   float R = texture2D(Texture0, oTexCoord0.xy).r;										\n"
		"   float G = texture2D(Texture0, oTexCoord1.xy).g;										\n"
		"   float B = texture2D(Texture0, oTexCoord2.xy).b;										\n"
		"   gl_FragColor = (oVignette*vec4(R,G,B,1));											\n"
		"}";

	pRender->InitShaders(vertexShader, pixelShader, &Shaders);

    for ( int eyeNum = 0; eyeNum < 2; eyeNum++ )
    {
        // Allocate mesh vertices, registering with renderer using the OVR vertex format.
        ovrDistortionMesh meshData;
        ovrHmd_CreateDistortionMesh(HMD, (ovrEyeType) eyeNum, eyeFov[eyeNum],
			                        ovrDistortionCap_Chromatic | ovrDistortionCap_TimeWarp, &meshData);
        MeshVBs[eyeNum] = *pRender->CreateBuffer();
        MeshVBs[eyeNum]->Data(Buffer_Vertex,meshData.pVertexData,sizeof(ovrDistortionVertex)*meshData.VertexCount);
        MeshIBs[eyeNum] = *pRender->CreateBuffer();
        MeshIBs[eyeNum]->Data(Buffer_Index,meshData.pIndexData,sizeof(unsigned short) * meshData.IndexCount);
        ovrHmd_DestroyDistortionMesh( &meshData );

		//Create eye render description for use later
		EyeRenderDesc[eyeNum] = ovrHmd_GetRenderDesc(HMD, (ovrEyeType) eyeNum,  eyeFov[eyeNum]);

		//Do scale and offset
		ovrHmd_GetRenderScaleAndOffset(eyeFov[eyeNum],RenderTargetSize, EyeRenderViewport[eyeNum], UVScaleOffset[eyeNum]);
	}

    #endif

    ovrHmd_SetEnabledCaps(HMD, ovrHmdCap_LowPersistence | ovrHmdCap_DynamicPrediction);

	// Start the sensor which informs of the Rift's pose and motion
    ovrHmd_ConfigureTracking(HMD,   ovrTrackingCap_Orientation |
                                    ovrTrackingCap_MagYawCorrection |
                                    ovrTrackingCap_Position, 0);

    // This creates lights and models.
  	pRoomScene = new Scene;
	PopulateRoomScene(pRoomScene, pRender);

    return (0);
}

//-------------------------------------------------------------------------------------
void ProcessAndRender()
{
    static ovrPosef eyeRenderPose[2]; 

	// Start timing
    #if SDK_RENDER
	ovrHmd_BeginFrame(HMD, 0); 
    #else
	ovrHmd_BeginFrameTiming(HMD, 0); 
    // Retrieve data useful for handling the Health and Safety Warning - unused, but here for reference
    ovrHSWDisplayState hswDisplayState;
    ovrHmd_GetHSWDisplayState(HMD, &hswDisplayState);
    #endif

	// Adjust eye position and rotation from controls, maintaining y position from HMD.
	static float    BodyYaw(3.141592f);
	static Vector3f HeadPos(0.0f, 1.6f, -5.0f);
    static ovrTrackingState HmdState;

    ovrVector3f hmdToEyeViewOffset[2] = { EyeRenderDesc[0].HmdToEyeViewOffset, EyeRenderDesc[1].HmdToEyeViewOffset };
    ovrHmd_GetEyePoses(HMD, 0, hmdToEyeViewOffset, eyeRenderPose, &HmdState);

	HeadPos.y = ovrHmd_GetFloat(HMD, OVR_KEY_EYE_HEIGHT, HeadPos.y);
	bool freezeEyeRender = Util_RespondToControls(BodyYaw, HeadPos, HmdState.HeadPose.ThePose.Orientation);

    pRender->BeginScene();
    
	// Render the two undistorted eye views into their render buffers.
    if (!freezeEyeRender) // freeze to debug for time warp
    {
        pRender->SetRenderTarget ( pRendertargetTexture );
        pRender->SetViewport (Recti(0,0, pRendertargetTexture->GetWidth(),
                                         pRendertargetTexture->GetHeight() ));  
        pRender->Clear();
		for (int eyeIndex = 0; eyeIndex < ovrEye_Count; eyeIndex++)
		{
            ovrEyeType eye = HMD->EyeRenderOrder[eyeIndex];

            // Get view and projection matrices
            Matrix4f rollPitchYaw       = Matrix4f::RotationY(BodyYaw);
            Matrix4f finalRollPitchYaw  = rollPitchYaw * Matrix4f(eyeRenderPose[eye].Orientation);
            Vector3f finalUp            = finalRollPitchYaw.Transform(Vector3f(0,1,0));
            Vector3f finalForward       = finalRollPitchYaw.Transform(Vector3f(0,0,-1));
            Vector3f shiftedEyePos      = HeadPos + rollPitchYaw.Transform(eyeRenderPose[eye].Position);
            Matrix4f view = Matrix4f::LookAtRH(shiftedEyePos, shiftedEyePos + finalForward, finalUp); 
			Matrix4f proj = ovrMatrix4f_Projection(EyeRenderDesc[eye].Fov, 0.01f, 10000.0f, true);

			pRender->SetViewport(Recti(EyeRenderViewport[eye]));
			pRender->SetProjection(proj);
			pRender->SetDepthMode(true, true);
			pRoomScene->Render(pRender, view);
		}
    }
    pRender->FinishScene();

    #if SDK_RENDER	// Let OVR do distortion rendering, Present and flush/sync
	ovrHmd_EndFrame(HMD, eyeRenderPose, &EyeTexture[0].Texture);
    #else
	// Clear screen
	pRender->SetDefaultRenderTarget();
	pRender->SetFullViewport();
	pRender->Clear(0.0f, 0.0f, 0.0f, 0.0f);

	// Setup shader
	ShaderFill distortionShaderFill(Shaders);
	distortionShaderFill.SetTexture(0, pRendertargetTexture);

	for(int eyeNum = 0; eyeNum < 2; eyeNum++)
	{
		// Get and set shader constants
		Shaders->SetUniform2f("EyeToSourceUVScale",   UVScaleOffset[eyeNum][0].x, UVScaleOffset[eyeNum][0].y);
		Shaders->SetUniform2f("EyeToSourceUVOffset",  UVScaleOffset[eyeNum][1].x, UVScaleOffset[eyeNum][1].y);
 		ovrMatrix4f timeWarpMatrices[2];
		ovrHmd_GetEyeTimewarpMatrices(HMD, (ovrEyeType)eyeNum, eyeRenderPose[eyeNum], timeWarpMatrices);
		Shaders->SetUniform4x4f("EyeRotationStart", timeWarpMatrices[0]);  //Nb transposed when set
		Shaders->SetUniform4x4f("EyeRotationEnd",   timeWarpMatrices[1]);  //Nb transposed when set
		// Perform distortion
		pRender->Render(&distortionShaderFill, MeshVBs[eyeNum], MeshIBs[eyeNum]);
	}
/*
    unsigned char latencyColor[3];
    ovrBool drawDk2LatencyQuad = ovrHmd_GetLatencyTest2DrawColor(HMD, latencyColor);
    if(drawDk2LatencyQuad)
    {
        const int latencyQuadSize = 20; // only needs to be 1-pixel, but larger helps visual debugging
        pRender->SetViewport(HMD->Resolution.w - latencyQuadSize, 0, latencyQuadSize, latencyQuadSize);
        pRender->Clear(latencyColor[0] / 255.0f, latencyColor[1] / 255.0f, latencyColor[2] / 255.0f, 0.0f);
    }
*/
	pRender->SetDefaultRenderTarget();

	pRender->Present( true ); // Vsync enabled

    // Only flush GPU for ExtendDesktop; not needed in Direct App Renering with Oculus driver.
    if (HMD->HmdCaps & ovrHmdCap_ExtendDesktop)
		pRender->WaitUntilGpuIdle();
  
	ovrHmd_EndFrameTiming(HMD);
    #endif
}

/*
void RenderFramePseudoCode()
{    
ovrFrame hmdFrameState = ovrHmd_BeginFrame(hmd); 

for (int eyeIndex = 0; eyeIndex < ovrEye_Count; eyeIndex++)
{
    ovrEyeType eye           = HMDDesc.EyeRenderOrder[eyeIndex];
    ovrPosef   eyeRenderPose = ovrHmd_BeginEyeRender(hmd, eye);

    RenderGameView(RenderViewports[eye], eyeRenderPose);

    ovrHmd_EndEyeRender(hmd, eye, &EyeTexture[eye].Texture);
}

// Let OVR do distortion rendering, Present and Flush+Sync.
ovrHmd_EndFrame(hmd);
}
*/ 

//-------------------------------------------------------------------------------------
void Release(void)
{
	if (pRendertargetTexture) pRendertargetTexture->Release();

    #if !SDK_RENDER
	for(int eyeNum = 0; eyeNum < 2; eyeNum++)
	{
		MeshVBs[eyeNum].Clear();
		MeshIBs[eyeNum].Clear();
	}
	if (Shaders)
	{
		Shaders->UnsetShader(Shader_Vertex);
		Shaders->UnsetShader(Shader_Pixel);
        Shaders->Release();
	}
    #endif

    ovrHmd_Destroy(HMD);
    HMD = 0;
    Util_ReleaseWindowAndGraphics(pRender);
    if (pRoomScene) delete pRoomScene;


    // No OVR functions involving memory are allowed after this.
    ovr_Shutdown(); 
}
